#include "aml.h"
#include "io.h"

/* ---- tunables / static storage ----------------------------------------
 * Everything here is a fixed-size arena - there's no malloc in this
 * kernel. Real DSDTs vary hugely in size; these caps are sized for the
 * kind of ACPI tables QEMU/Bochs/VirtualBox hand a guest, which is
 * what this OS mainly targets (see the power_off() comment in
 * kernel.c). A big real-hardware DSDT may exceed AML_MAX_NODES; in
 * that case indexing just stops early rather than overflowing
 * anything - whatever was indexed before the limit is still usable.
 * Real laptop DSDTs (this includes most Lenovo ThinkPad/IdeaPad
 * firmware) routinely define several thousand named objects across
 * WMI, EC, thermal, and power-management code before ever reaching
 * the PNP0C0A battery device, so a cap sized for VM firmware silently
 * runs out and drops the battery (and everything after it) from the
 * namespace with no error. Sized generously for real hardware; this
 * is static BSS, not heap, so the cost is a fixed ~350KB of image
 * size, not runtime allocation. */
#define AML_MAX_NODES        16384
#define AML_MAX_DEPTH        24
#define AML_MAX_CALL_DEPTH   8
#define AML_MAX_LOOP_ITERS   200000
#define AML_MAX_PKG_POOL     96    /* scratch pool for one aml_evaluate() call */
#define AML_MAX_STATIC_PKG_POOL 1024 /* permanent pool for Name()-time constant packages (e.g. _CID lists) */
#define NAME_BUF_SZ           80

enum aml_node_kind {
    NODE_ROOT = 0,
    NODE_DEVICE,
    NODE_NAME,
    NODE_METHOD,
    NODE_OPREGION,
    NODE_FIELDUNIT,
    NODE_OTHER,
};

struct aml_node {
    char name[4];
    struct aml_node* parent;
    enum aml_node_kind kind;

    /* NODE_NAME */
    struct aml_value value;
    int value_valid;

    /* NODE_METHOD */
    const uint8_t* method_start;
    uint32_t method_len;
    uint8_t method_argc;

    /* NODE_OPREGION */
    uint8_t region_space;
    uint64_t region_offset;

    /* NODE_FIELDUNIT */
    struct aml_node* region;
    uint32_t bit_offset;
    uint32_t bit_len;
};

enum exec_status { EXEC_NORMAL = 0, EXEC_RETURN, EXEC_BREAK, EXEC_CONTINUE, EXEC_FAILED };

struct aml_frame {
    struct aml_node* scope;
    struct aml_value locals[8];
    struct aml_value args[7];
};

struct eval_result {
    enum exec_status status;
    struct aml_value value;
};

static struct aml_node g_nodes[AML_MAX_NODES];
static int g_node_count = 0;
static struct aml_node* g_root = 0;

static struct aml_value g_pkg_pool[AML_MAX_PKG_POOL];
static int g_pkg_pool_used = 0;
static struct aml_value g_static_pkg_pool[AML_MAX_STATIC_PKG_POOL];
static int g_static_pkg_pool_used = 0;
static int g_indexing_mode = 0;
static int g_call_depth = 0;

static struct eval_result eval_term(const uint8_t** cur, const uint8_t* limit, struct aml_frame* frame);
static struct eval_result exec_term_list(const uint8_t* start, const uint8_t* end, struct aml_frame* frame);

/* ---- tiny value constructors ------------------------------------------ */

static struct aml_value mkint(uint64_t v) {
    struct aml_value r;
    r.type = AML_INTEGER; r.integer = v; r.str = 0; r.str_len = 0; r.pkg_elems = 0; r.pkg_count = 0;
    return r;
}
static struct aml_value mkstr(const char* s, uint32_t len) {
    struct aml_value r;
    r.type = AML_STRING; r.integer = 0; r.str = s; r.str_len = len; r.pkg_elems = 0; r.pkg_count = 0;
    return r;
}
static struct aml_value mkfail(void) {
    struct aml_value r;
    r.type = AML_UNINITIALIZED; r.integer = 0; r.str = 0; r.str_len = 0; r.pkg_elems = 0; r.pkg_count = 0;
    return r;
}

/* ---- byte-stream helpers ------------------------------------------------ */

static uint16_t rd_u16(const uint8_t** p) {
    uint16_t v = (uint16_t)((*p)[0] | ((*p)[1] << 8));
    *p += 2; return v;
}
static uint32_t rd_u32(const uint8_t** p) {
    uint32_t v = (uint32_t)((*p)[0] | ((*p)[1] << 8) | ((*p)[2] << 16) | ((*p)[3] << 24));
    *p += 4; return v;
}
static uint64_t rd_u64(const uint8_t** p) {
    uint64_t lo = rd_u32(p);
    uint64_t hi = rd_u32(p);
    return lo | (hi << 32);
}

static int is_lead_name_char(uint8_t c) { return c == '_' || (c >= 'A' && c <= 'Z'); }

/* AML PkgLength (ACPI spec 20.2.4). The returned length includes the
 * bytes used to encode the PkgLength itself, so callers compute the
 * end of the enclosed block as (pointer-before-parsing + length). */
static uint32_t parse_pkglength(const uint8_t** p) {
    uint8_t lead = **p;
    uint8_t count = (lead >> 6) & 0x3;
    (*p)++;
    uint32_t length;
    if (count == 0) {
        length = lead & 0x3F;
    } else {
        length = lead & 0x0F;
        for (int i = 0; i < count; i++) {
            length |= ((uint32_t)(**p)) << (4 + 8 * i);
            (*p)++;
        }
    }
    return length;
}

/* NameString (ACPI spec 20.2.2), normalized into a '.'-separated,
 * dot-free-per-segment string like "\_SB.PCI0.BAT0" ready for
 * resolve_name()/create_or_get_path(). */
static void parse_name_string(const uint8_t** cur, char* out, int outsz) {
    int pos = 0;
    if (**cur == '\\') {
        if (pos < outsz - 1) out[pos++] = '\\';
        (*cur)++;
    } else {
        while (**cur == '^') {
            if (pos < outsz - 1) out[pos++] = '^';
            (*cur)++;
        }
    }
    uint8_t b = **cur;
    if (b == 0x00) {
        (*cur)++;
    } else if (b == 0x2E) { /* DualNamePrefix */
        (*cur)++;
        for (int seg = 0; seg < 2; seg++) {
            if (seg > 0 && pos < outsz - 1) out[pos++] = '.';
            for (int i = 0; i < 4 && pos < outsz - 1; i++) out[pos++] = (char)(*cur)[i];
            (*cur) += 4;
        }
    } else if (b == 0x2F) { /* MultiNamePrefix */
        (*cur)++;
        uint8_t segcount = **cur; (*cur)++;
        for (int seg = 0; seg < segcount; seg++) {
            if (seg > 0 && pos < outsz - 1) out[pos++] = '.';
            for (int i = 0; i < 4 && pos < outsz - 1; i++) out[pos++] = (char)(*cur)[i];
            (*cur) += 4;
        }
    } else if (is_lead_name_char(b)) {
        for (int i = 0; i < 4 && pos < outsz - 1; i++) out[pos++] = (char)(*cur)[i];
        (*cur) += 4;
    }
    out[pos] = 0;
}

/* ---- namespace tree ----------------------------------------------------- */

static struct aml_node* node_alloc(void) {
    if (g_node_count >= AML_MAX_NODES) return 0;
    struct aml_node* n = &g_nodes[g_node_count++];
    n->name[0] = n->name[1] = n->name[2] = n->name[3] = '_';
    n->parent = 0;
    n->kind = NODE_OTHER;
    n->value = mkfail();
    n->value_valid = 0;
    n->method_start = 0; n->method_len = 0; n->method_argc = 0;
    n->region_space = 0; n->region_offset = 0;
    n->region = 0; n->bit_offset = 0; n->bit_len = 0;
    return n;
}

static struct aml_node* find_child(struct aml_node* parent, const char seg[4]) {
    for (int i = 0; i < g_node_count; i++) {
        struct aml_node* n = &g_nodes[i];
        if (n->parent == parent && n->name[0] == seg[0] && n->name[1] == seg[1] &&
            n->name[2] == seg[2] && n->name[3] == seg[3]) return n;
    }
    return 0;
}

static struct aml_node* create_or_get_path(struct aml_node* scope, const char* path) {
    const char* p = path;
    struct aml_node* base = scope;
    if (*p == '\\') { base = g_root; p++; }
    else { while (*p == '^') { if (base && base->parent) base = base->parent; p++; } }
    if (!base) return 0;
    if (*p == 0) return base;
    struct aml_node* cur = base;
    while (*p) {
        char seg[4]; for (int i = 0; i < 4; i++) seg[i] = p[i];
        p += 4;
        if (*p == '.') p++;
        struct aml_node* n = find_child(cur, seg);
        if (!n) {
            n = node_alloc();
            if (!n) return 0;
            n->parent = cur;
            n->name[0] = seg[0]; n->name[1] = seg[1]; n->name[2] = seg[2]; n->name[3] = seg[3];
        }
        cur = n;
    }
    return cur;
}

/* Single NameSeg lookups walk up enclosing scopes if not found locally
 * (the ACPI "namespace search rule"); multi-segment and \-rooted or
 * ^-relative paths resolve their first segment the same way but then
 * only look at direct children from there on. */
static struct aml_node* resolve_name(struct aml_node* scope, const char* path) {
    const char* p = path;
    struct aml_node* base = scope;
    int walkup_allowed = 1;
    if (*p == '\\') { base = g_root; p++; walkup_allowed = 0; }
    else {
        int carets = 0;
        while (*p == '^') { carets++; p++; }
        if (carets > 0) {
            for (int i = 0; i < carets && base; i++) base = base->parent;
            if (!base) return 0;
            walkup_allowed = 0;
        }
    }
    if (!base) return 0;
    if (*p == 0) return base;
    struct aml_node* cur_scope = base;
    int first = 1;
    while (*p) {
        char seg[4]; for (int i = 0; i < 4; i++) seg[i] = p[i];
        p += 4;
        if (*p == '.') p++;
        struct aml_node* found = 0;
        if (first && walkup_allowed) {
            struct aml_node* s = cur_scope;
            while (s && !(found = find_child(s, seg))) s = s->parent;
        } else {
            found = find_child(cur_scope, seg);
        }
        if (!found) return 0;
        cur_scope = found;
        first = 0;
    }
    return cur_scope;
}

static struct aml_node* find_named_child(struct aml_node* parent, const char* name4) {
    char seg[4] = { name4[0], name4[1], name4[2], name4[3] };
    return find_child(parent, seg);
}

/* ---- operation region access (SystemMemory / SystemIO / EmbeddedControl) */

enum { RSPACE_SYSTEM_MEMORY = 0, RSPACE_SYSTEM_IO = 1, RSPACE_EC = 3 };

#define EC_DATA_PORT 0x62
#define EC_CMD_PORT  0x66
#define EC_SC_OBF    0x01
#define EC_SC_IBF    0x02

static int ec_wait_ibf_clear(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(EC_CMD_PORT) & EC_SC_IBF)) return 1;
    return 0;
}
static int ec_wait_obf_set(void) {
    for (int i = 0; i < 100000; i++) if (inb(EC_CMD_PORT) & EC_SC_OBF) return 1;
    return 0;
}
static uint8_t ec_read_byte(uint8_t addr) {
    if (!ec_wait_ibf_clear()) return 0xFF;
    outb(EC_CMD_PORT, 0x80); /* RD_EC */
    if (!ec_wait_ibf_clear()) return 0xFF;
    outb(EC_DATA_PORT, addr);
    if (!ec_wait_obf_set()) return 0xFF;
    return inb(EC_DATA_PORT);
}
static void ec_write_byte(uint8_t addr, uint8_t val) {
    if (!ec_wait_ibf_clear()) return;
    outb(EC_CMD_PORT, 0x81); /* WR_EC */
    if (!ec_wait_ibf_clear()) return;
    outb(EC_DATA_PORT, addr);
    if (!ec_wait_ibf_clear()) return;
    outb(EC_DATA_PORT, val);
}

static uint8_t region_read_byte(struct aml_node* region, uint64_t byte_index) {
    if (!region) return 0xFF;
    uint64_t addr = region->region_offset + byte_index;
    switch (region->region_space) {
        case RSPACE_SYSTEM_MEMORY: return *(volatile uint8_t*)(uintptr_t)addr;
        case RSPACE_SYSTEM_IO:     return inb((uint16_t)addr);
        case RSPACE_EC:            return ec_read_byte((uint8_t)addr);
        default:                   return 0xFF; /* unsupported region space (e.g. PCI config, SMBus) */
    }
}
static void region_write_byte(struct aml_node* region, uint64_t byte_index, uint8_t val) {
    if (!region) return;
    uint64_t addr = region->region_offset + byte_index;
    switch (region->region_space) {
        case RSPACE_SYSTEM_MEMORY: *(volatile uint8_t*)(uintptr_t)addr = val; break;
        case RSPACE_SYSTEM_IO:     outb((uint16_t)addr, val); break;
        case RSPACE_EC:            ec_write_byte((uint8_t)addr, val); break;
        default: break;
    }
}

static uint64_t region_read_bits(struct aml_node* region, uint32_t bit_off, uint32_t bit_len) {
    if (bit_len == 0) return 0;
    uint32_t byte_off = bit_off / 8;
    uint32_t bit_shift = bit_off % 8;
    uint32_t nbytes = (bit_shift + bit_len + 7) / 8;
    if (nbytes > 8) nbytes = 8;
    uint64_t raw = 0;
    for (uint32_t i = 0; i < nbytes; i++) raw |= ((uint64_t)region_read_byte(region, byte_off + i)) << (8 * i);
    raw >>= bit_shift;
    if (bit_len < 64) raw &= (((uint64_t)1 << bit_len) - 1);
    return raw;
}
static void region_write_bits(struct aml_node* region, uint32_t bit_off, uint32_t bit_len, uint64_t val) {
    /* Byte-granular read-modify-write; fine for the byte-aligned fields
     * typical of EC battery AML, not a general bit-packer. */
    if (bit_len == 0) return;
    uint32_t byte_off = bit_off / 8;
    uint32_t bit_shift = bit_off % 8;
    uint32_t nbytes = (bit_shift + bit_len + 7) / 8;
    if (nbytes > 8) nbytes = 8;
    uint64_t mask = (bit_len < 64) ? (((uint64_t)1 << bit_len) - 1) : ~0ULL;
    uint64_t cur = 0;
    for (uint32_t i = 0; i < nbytes; i++) cur |= ((uint64_t)region_read_byte(region, byte_off + i)) << (8 * i);
    cur &= ~(mask << bit_shift);
    cur |= (val & mask) << bit_shift;
    for (uint32_t i = 0; i < nbytes; i++) region_write_byte(region, byte_off + i, (uint8_t)(cur >> (8 * i)));
}

static struct aml_value node_read_value(struct aml_node* node) {
    if (!node) return mkfail();
    switch (node->kind) {
        case NODE_NAME:      return node->value_valid ? node->value : mkfail();
        case NODE_FIELDUNIT:  return mkint(region_read_bits(node->region, node->bit_offset, node->bit_len));
        default:              return mkfail();
    }
}
static void node_write_value(struct aml_node* node, struct aml_value v) {
    if (!node) return;
    if (node->kind == NODE_FIELDUNIT) {
        region_write_bits(node->region, node->bit_offset, node->bit_len, v.integer);
    } else {
        node->kind = NODE_NAME;
        node->value = v;
        node->value_valid = 1;
    }
}

/* ---- SuperName / Target handling --------------------------------------- */

enum target_kind { TGT_NONE, TGT_LOCAL, TGT_ARG, TGT_NODE, TGT_DEBUG };
struct target_ref { enum target_kind kind; int index; struct aml_node* node; };

static struct target_ref parse_target(const uint8_t** cur, struct aml_frame* frame) {
    struct target_ref t; t.kind = TGT_NONE; t.index = 0; t.node = 0;
    uint8_t b = **cur;
    if (b == 0x00) { (*cur)++; return t; }
    if (b >= 0x60 && b <= 0x67) { (*cur)++; t.kind = TGT_LOCAL; t.index = b - 0x60; return t; }
    if (b >= 0x68 && b <= 0x6E) { (*cur)++; t.kind = TGT_ARG; t.index = b - 0x68; return t; }
    if (b == 0x5B && (*cur)[1] == 0x31) { (*cur) += 2; t.kind = TGT_DEBUG; return t; }
    char path[NAME_BUF_SZ];
    parse_name_string(cur, path, sizeof(path));
    t.kind = TGT_NODE;
    t.node = resolve_name(frame->scope, path);
    return t;
}
static void store_target(struct target_ref* t, struct aml_value v, struct aml_frame* frame) {
    switch (t->kind) {
        case TGT_LOCAL: frame->locals[t->index] = v; break;
        case TGT_ARG:   frame->args[t->index] = v; break;
        case TGT_NODE:  if (t->node) node_write_value(t->node, v); break;
        default: break;
    }
}
static struct aml_value read_target(struct target_ref* t, struct aml_frame* frame) {
    switch (t->kind) {
        case TGT_LOCAL: return frame->locals[t->index];
        case TGT_ARG:   return frame->args[t->index];
        case TGT_NODE:  return node_read_value(t->node);
        default:        return mkint(0);
    }
}

/* ---- arithmetic / compare helpers --------------------------------------- */

static uint64_t fn_add(uint64_t a, uint64_t b) { return a + b; }
static uint64_t fn_sub(uint64_t a, uint64_t b) { return a - b; }
static uint64_t fn_mul(uint64_t a, uint64_t b) { return a * b; }
static uint64_t fn_and(uint64_t a, uint64_t b) { return a & b; }
static uint64_t fn_or(uint64_t a, uint64_t b)  { return a | b; }
static uint64_t fn_xor(uint64_t a, uint64_t b) { return a ^ b; }
static uint64_t fn_nand(uint64_t a, uint64_t b) { return ~(a & b); }
static uint64_t fn_nor(uint64_t a, uint64_t b)  { return ~(a | b); }
static uint64_t fn_shl(uint64_t a, uint64_t b) { return (b >= 64) ? 0 : (a << b); }
static uint64_t fn_shr(uint64_t a, uint64_t b) { return (b >= 64) ? 0 : (a >> b); }
static uint64_t fn_mod(uint64_t a, uint64_t b) { return b ? (a % b) : 0; }

static struct eval_result binop2(const uint8_t** cur, const uint8_t* limit, struct aml_frame* frame,
                                  uint64_t (*fn)(uint64_t, uint64_t)) {
    struct eval_result a = eval_term(cur, limit, frame); if (a.status != EXEC_NORMAL) return a;
    struct eval_result b = eval_term(cur, limit, frame); if (b.status != EXEC_NORMAL) return b;
    struct target_ref t = parse_target(cur, frame);
    struct aml_value v = mkint(fn(a.value.integer, b.value.integer));
    store_target(&t, v, frame);
    struct eval_result r; r.status = EXEC_NORMAL; r.value = v; return r;
}

static struct eval_result cmpop(const uint8_t** cur, const uint8_t* limit, struct aml_frame* frame, int mode) {
    /* mode: 0=Equal, 1=Less, 2=Greater */
    struct eval_result a = eval_term(cur, limit, frame); if (a.status != EXEC_NORMAL) return a;
    struct eval_result b = eval_term(cur, limit, frame); if (b.status != EXEC_NORMAL) return b;
    int res;
    if (a.value.type == AML_STRING && b.value.type == AML_STRING) {
        uint32_t n = a.value.str_len < b.value.str_len ? a.value.str_len : b.value.str_len;
        int c = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (a.value.str[i] != b.value.str[i]) { c = (a.value.str[i] < b.value.str[i]) ? -1 : 1; break; }
        }
        if (c == 0) c = (a.value.str_len < b.value.str_len) ? -1 : (a.value.str_len > b.value.str_len ? 1 : 0);
        res = (mode == 0) ? (c == 0) : (mode == 1) ? (c < 0) : (c > 0);
    } else {
        uint64_t av = a.value.integer, bv = b.value.integer;
        res = (mode == 0) ? (av == bv) : (mode == 1) ? (av < bv) : (av > bv);
    }
    struct eval_result r; r.status = EXEC_NORMAL; r.value = mkint(res ? 1 : 0); return r;
}

/* ---- method invocation --------------------------------------------------- */

static struct aml_value call_method(struct aml_node* node, struct aml_value* args, int argc) {
    if (!node || node->kind != NODE_METHOD) return mkfail();
    if (g_call_depth >= AML_MAX_CALL_DEPTH) return mkfail();
    g_call_depth++;
    struct aml_frame f;
    f.scope = node;
    for (int i = 0; i < 8; i++) f.locals[i] = mkint(0);
    for (int i = 0; i < 7; i++) f.args[i] = (i < argc) ? args[i] : mkint(0);
    struct eval_result r = exec_term_list(node->method_start, node->method_start + node->method_len, &f);
    g_call_depth--;
    if (r.status == EXEC_RETURN) return r.value;
    if (r.status == EXEC_FAILED) return mkfail();
    return mkint(0); /* fell off the end without an explicit Return -> implicit Return(Zero) */
}

/* ---- the evaluator -------------------------------------------------------
 * Handles one AML term (an opcode plus whatever operands it takes) and
 * advances *cur past it. Used both to execute method bodies and (in
 * indexing mode) to parse-and-capture the constant DataRefObject that
 * follows a Name() declaration. See aml.h for exactly what's covered. */
static struct eval_result eval_term(const uint8_t** cur, const uint8_t* limit, struct aml_frame* frame) {
    struct eval_result r; r.status = EXEC_NORMAL; r.value = mkint(0);
    if (*cur >= limit) { r.status = EXEC_FAILED; return r; }
    uint8_t op = **cur; (*cur)++;

    switch (op) {
    case 0x00: r.value = mkint(0); return r;               /* ZeroOp */
    case 0x01: r.value = mkint(1); return r;                /* OneOp */
    case 0xFF: r.value = mkint(~0ULL); return r;             /* OnesOp */
    case 0x0A: r.value = mkint(**cur); (*cur)++; return r;   /* BytePrefix */
    case 0x0B: r.value = mkint(rd_u16(cur)); return r;       /* WordPrefix */
    case 0x0C: r.value = mkint(rd_u32(cur)); return r;       /* DWordPrefix */
    case 0x0E: r.value = mkint(rd_u64(cur)); return r;       /* QWordPrefix */
    case 0x0D: {                                             /* StringPrefix */
        const char* s = (const char*)*cur;
        uint32_t len = 0;
        while (*cur + len < limit && s[len] != 0) len++;
        (*cur) += len + 1;
        r.value = mkstr(s, len);
        return r;
    }
    case 0x06: {                                             /* AliasOp - not modeled, just skip both names */
        char a[NAME_BUF_SZ], b[NAME_BUF_SZ];
        parse_name_string(cur, a, sizeof(a));
        parse_name_string(cur, b, sizeof(b));
        return r;
    }
    case 0x08: {                                             /* NameOp (nested inside a method/If/While body) */
        char path[NAME_BUF_SZ]; parse_name_string(cur, path, sizeof(path));
        struct aml_node* n = create_or_get_path(frame->scope, path);
        struct eval_result v = eval_term(cur, limit, frame);
        if (v.status != EXEC_NORMAL) return v;
        if (n) { n->kind = NODE_NAME; n->value = v.value; n->value_valid = 1; }
        return r;
    }
    case 0x10: {                                             /* ScopeOp (rare mid-body) */
        const uint8_t* pkg_start = *cur;
        uint32_t len = parse_pkglength(cur);
        const uint8_t* body_end = pkg_start + len;
        char path[NAME_BUF_SZ]; parse_name_string(cur, path, sizeof(path));
        struct aml_node* n = create_or_get_path(frame->scope, path);
        struct aml_frame sub = *frame; sub.scope = n ? n : frame->scope;
        struct eval_result br = exec_term_list(*cur, body_end, &sub);
        *cur = body_end;
        if (br.status == EXEC_RETURN || br.status == EXEC_FAILED) return br;
        return r;
    }
    case 0x12: {                                             /* PackageOp */
        const uint8_t* pkg_start = *cur;
        uint32_t len = parse_pkglength(cur);
        const uint8_t* body_end = pkg_start + len;
        (*cur)++; /* NumElements byte - element list below is self-terminating via body_end */
        struct aml_value* pool; int* used; int cap;
        if (g_indexing_mode) { pool = g_static_pkg_pool; used = &g_static_pkg_pool_used; cap = AML_MAX_STATIC_PKG_POOL; }
        else { pool = g_pkg_pool; used = &g_pkg_pool_used; cap = AML_MAX_PKG_POOL; }
        int base = *used, count = 0;
        while (*cur < body_end && *used < cap) {
            struct eval_result e = eval_term(cur, body_end, frame);
            if (e.status != EXEC_NORMAL) { *cur = body_end; break; }
            pool[(*used)++] = e.value;
            count++;
        }
        *cur = body_end;
        r.value.type = AML_PACKAGE; r.value.pkg_elems = &pool[base]; r.value.pkg_count = (uint32_t)count;
        return r;
    }
    case 0x13: {                                             /* VarPackageOp */
        const uint8_t* pkg_start = *cur;
        uint32_t len = parse_pkglength(cur);
        const uint8_t* body_end = pkg_start + len;
        struct eval_result nc = eval_term(cur, body_end, frame); /* NumElements TermArg, value unused - element list is self-terminating */
        if (nc.status != EXEC_NORMAL) { *cur = body_end; return nc; }
        struct aml_value* pool; int* used; int cap;
        if (g_indexing_mode) { pool = g_static_pkg_pool; used = &g_static_pkg_pool_used; cap = AML_MAX_STATIC_PKG_POOL; }
        else { pool = g_pkg_pool; used = &g_pkg_pool_used; cap = AML_MAX_PKG_POOL; }
        int base = *used, count = 0;
        while (*cur < body_end && *used < cap) {
            struct eval_result e = eval_term(cur, body_end, frame);
            if (e.status != EXEC_NORMAL) { *cur = body_end; break; }
            pool[(*used)++] = e.value;
            count++;
        }
        *cur = body_end;
        r.value.type = AML_PACKAGE; r.value.pkg_elems = &pool[base]; r.value.pkg_count = (uint32_t)count;
        return r;
    }
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
        r.value = frame->locals[op - 0x60]; return r;        /* Local0-7 */
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E:
        r.value = frame->args[op - 0x68]; return r;           /* Arg0-6 */

    case 0x70: {                                             /* Store(Operand, Target) */
        struct eval_result src = eval_term(cur, limit, frame);
        if (src.status != EXEC_NORMAL) return src;
        struct target_ref t = parse_target(cur, frame);
        store_target(&t, src.value, frame);
        r.value = src.value;
        return r;
    }
    case 0x72: return binop2(cur, limit, frame, fn_add);
    case 0x74: return binop2(cur, limit, frame, fn_sub);
    case 0x77: return binop2(cur, limit, frame, fn_mul);
    case 0x79: return binop2(cur, limit, frame, fn_shl);
    case 0x7A: return binop2(cur, limit, frame, fn_shr);
    case 0x7B: return binop2(cur, limit, frame, fn_and);
    case 0x7C: return binop2(cur, limit, frame, fn_nand);
    case 0x7D: return binop2(cur, limit, frame, fn_or);
    case 0x7E: return binop2(cur, limit, frame, fn_nor);
    case 0x7F: return binop2(cur, limit, frame, fn_xor);
    case 0x85: return binop2(cur, limit, frame, fn_mod);

    case 0x78: {                                             /* Divide(Dividend, Divisor, Remainder, Quotient) */
        struct eval_result a = eval_term(cur, limit, frame); if (a.status != EXEC_NORMAL) return a;
        struct eval_result b = eval_term(cur, limit, frame); if (b.status != EXEC_NORMAL) return b;
        struct target_ref rem_t = parse_target(cur, frame);
        struct target_ref quo_t = parse_target(cur, frame);
        uint64_t divisor = b.value.integer;
        uint64_t quotient = divisor ? (a.value.integer / divisor) : 0;
        uint64_t remainder = divisor ? (a.value.integer % divisor) : 0;
        store_target(&rem_t, mkint(remainder), frame);
        store_target(&quo_t, mkint(quotient), frame);
        r.value = mkint(quotient);
        return r;
    }
    case 0x75: {                                             /* Increment(SuperName) */
        struct target_ref t = parse_target(cur, frame);
        struct aml_value v = mkint(read_target(&t, frame).integer + 1);
        store_target(&t, v, frame);
        r.value = v; return r;
    }
    case 0x76: {                                             /* Decrement(SuperName) */
        struct target_ref t = parse_target(cur, frame);
        struct aml_value v = mkint(read_target(&t, frame).integer - 1);
        store_target(&t, v, frame);
        r.value = v; return r;
    }
    case 0x80: {                                             /* Not(Operand, Target) */
        struct eval_result a = eval_term(cur, limit, frame); if (a.status != EXEC_NORMAL) return a;
        struct target_ref t = parse_target(cur, frame);
        struct aml_value v = mkint(~a.value.integer);
        store_target(&t, v, frame);
        r.value = v; return r;
    }
    case 0x86: {                                             /* Notify(Object, Value) - no-op */
        parse_target(cur, frame);
        struct eval_result val = eval_term(cur, limit, frame); if (val.status != EXEC_NORMAL) return val;
        return r;
    }
    case 0x90: {                                             /* LAnd */
        struct eval_result a = eval_term(cur, limit, frame); if (a.status != EXEC_NORMAL) return a;
        struct eval_result b = eval_term(cur, limit, frame); if (b.status != EXEC_NORMAL) return b;
        r.value = mkint((a.value.integer != 0) && (b.value.integer != 0)); return r;
    }
    case 0x91: {                                             /* LOr */
        struct eval_result a = eval_term(cur, limit, frame); if (a.status != EXEC_NORMAL) return a;
        struct eval_result b = eval_term(cur, limit, frame); if (b.status != EXEC_NORMAL) return b;
        r.value = mkint((a.value.integer != 0) || (b.value.integer != 0)); return r;
    }
    case 0x92: {                                             /* LNot */
        struct eval_result a = eval_term(cur, limit, frame); if (a.status != EXEC_NORMAL) return a;
        r.value = mkint(a.value.integer == 0 ? 1 : 0); return r;
    }
    case 0x93: return cmpop(cur, limit, frame, 0);            /* LEqual */
    case 0x94: return cmpop(cur, limit, frame, 2);            /* LGreater */
    case 0x95: return cmpop(cur, limit, frame, 1);            /* LLess */

    case 0xA0: {                                             /* If(Predicate){...} [Else{...}] */
        const uint8_t* pkg_start = *cur;
        uint32_t len = parse_pkglength(cur);
        const uint8_t* body_end = pkg_start + len;
        struct eval_result cond = eval_term(cur, body_end, frame);
        if (cond.status != EXEC_NORMAL) { *cur = body_end; return cond; }
        struct eval_result br; br.status = EXEC_NORMAL; br.value = mkint(0);
        if (cond.value.integer != 0) br = exec_term_list(*cur, body_end, frame);
        *cur = body_end;
        if (*cur < limit && **cur == 0xA1) {
            (*cur)++;
            const uint8_t* epkg_start = *cur;
            uint32_t elen = parse_pkglength(cur);
            const uint8_t* ebody_end = epkg_start + elen;
            if (cond.value.integer == 0) br = exec_term_list(*cur, ebody_end, frame);
            *cur = ebody_end;
        }
        return br;
    }
    case 0xA1: {                                             /* stray Else with no matching If */
        const uint8_t* pkg_start = *cur;
        uint32_t len = parse_pkglength(cur);
        *cur = pkg_start + len;
        return r;
    }
    case 0xA2: {                                             /* While(Predicate){...} */
        const uint8_t* pkg_start = *cur;
        uint32_t len = parse_pkglength(cur);
        const uint8_t* body_end = pkg_start + len;
        const uint8_t* pred_start = *cur;
        struct eval_result final_r; final_r.status = EXEC_NORMAL; final_r.value = mkint(0);
        uint32_t guard = 0;
        for (;;) {
            if (++guard > AML_MAX_LOOP_ITERS) { final_r.status = EXEC_FAILED; break; }
            const uint8_t* pp = pred_start;
            struct eval_result cond = eval_term(&pp, body_end, frame);
            if (cond.status != EXEC_NORMAL) { final_r = cond; break; }
            if (cond.value.integer == 0) break;
            struct eval_result br = exec_term_list(pp, body_end, frame);
            if (br.status == EXEC_BREAK) break;
            if (br.status == EXEC_RETURN || br.status == EXEC_FAILED) { final_r = br; break; }
        }
        *cur = body_end;
        return final_r;
    }
    case 0xA3: return r;                                     /* Noop */
    case 0xA4: {                                             /* Return(Arg) */
        struct eval_result v = eval_term(cur, limit, frame);
        if (v.status != EXEC_NORMAL) return v;
        r.status = EXEC_RETURN; r.value = v.value;
        return r;
    }
    case 0xA5: r.status = EXEC_BREAK; return r;               /* Break */
    case 0x9F: r.status = EXEC_CONTINUE; return r;            /* Continue */
    case 0xCC: return r;                                      /* BreakPoint - no-op */

    case 0x5B: {                                             /* ExtOpPrefix */
        if (*cur >= limit) { r.status = EXEC_FAILED; return r; }
        uint8_t op2 = **cur; (*cur)++;
        switch (op2) {
        case 0x21: {                                          /* Stall(Operand) - no-op */
            struct eval_result a = eval_term(cur, limit, frame); if (a.status != EXEC_NORMAL) return a;
            return r;
        }
        case 0x22: {                                          /* Sleep(Operand) - no-op; can't block the boot loop */
            struct eval_result a = eval_term(cur, limit, frame); if (a.status != EXEC_NORMAL) return a;
            return r;
        }
        case 0x31: r.value = mkint(0); return r;              /* DebugObj read as a bare expression */
        case 0x80: {                                          /* OpRegionOp (nested; rare but handle it) */
            char path[NAME_BUF_SZ]; parse_name_string(cur, path, sizeof(path));
            struct aml_node* n = create_or_get_path(frame->scope, path);
            uint8_t space = **cur; (*cur)++;
            struct eval_result off = eval_term(cur, limit, frame); if (off.status != EXEC_NORMAL) return off;
            struct eval_result len_ = eval_term(cur, limit, frame); if (len_.status != EXEC_NORMAL) return len_;
            if (n) { n->kind = NODE_OPREGION; n->region_space = space; n->region_offset = off.value.integer; }
            return r;
        }
        default:
            r.status = EXEC_FAILED;
            return r;
        }
    }

    default:
        if (op == '\\' || op == '^' || op == 0x2E || op == 0x2F || is_lead_name_char(op)) {
            (*cur)--;
            char path[NAME_BUF_SZ];
            parse_name_string(cur, path, sizeof(path));
            struct aml_node* n = resolve_name(frame->scope, path);
            if (!n) {
                /* _OSI ("Interface Name") is a predefined control
                 * method (ACPI spec 5.7.4.5.1) evaluated natively by
                 * the OS - it's never a named AML object, so it will
                 * never resolve. Real DSDTs (especially on laptops)
                 * gate large amounts of functional code, including EC
                 * and battery access paths, behind
                 * If (_OSI ("Windows ...")): failing this call instead
                 * of answering it sends every such check down the
                 * "unsupported OS" branch, which firmware vendors
                 * barely test. Claim support unconditionally, same as
                 * what actually running under Windows gets. */
                const char* seg = path;
                while (*seg == '\\' || *seg == '^') seg++;
                int is_osi = (seg[0] == '_' && seg[1] == 'O' && seg[2] == 'S' && seg[3] == 'I' &&
                              seg[4] == '\0');
                if (is_osi) {
                    struct eval_result arg = eval_term(cur, limit, frame); /* consume+discard the interface-name String */
                    if (arg.status != EXEC_NORMAL) return arg;
                    r.value = mkint(1);
                    return r;
                }
                r.status = EXEC_FAILED; return r;
            }
            if (n->kind == NODE_METHOD) {
                struct aml_value argvals[7];
                int argc = n->method_argc; if (argc > 7) argc = 7;
                for (int i = 0; i < argc; i++) {
                    struct eval_result av = eval_term(cur, limit, frame);
                    if (av.status != EXEC_NORMAL) { r.status = EXEC_FAILED; return r; }
                    argvals[i] = av.value;
                }
                r.value = call_method(n, argvals, argc);
                if (r.value.type == AML_UNINITIALIZED) r.status = EXEC_FAILED;
                return r;
            }
            r.value = node_read_value(n);
            if (r.value.type == AML_UNINITIALIZED) r.status = EXEC_FAILED;
            return r;
        }
        r.status = EXEC_FAILED;
        return r;
    }
}

static struct eval_result exec_term_list(const uint8_t* start, const uint8_t* end, struct aml_frame* frame) {
    const uint8_t* p = start;
    struct eval_result r; r.status = EXEC_NORMAL; r.value = mkint(0);
    while (p < end) {
        r = eval_term(&p, end, frame);
        if (r.status != EXEC_NORMAL) return r;
    }
    return r;
}

/* ---- namespace indexing --------------------------------------------------
 * Walks an ObjectList (the DSDT/SSDT body, or the inside of a Scope/
 * Device/Processor/PowerResource/ThermalZone) purely to build the
 * namespace tree - it does not execute method bodies (those are only
 * evaluated on demand by aml_evaluate()). Anything that isn't one of
 * the namespace-defining opcodes falls back to the general evaluator
 * just to keep the cursor advancing correctly; if that also can't
 * make sense of it, indexing of that particular list stops there
 * (whatever was already found stays usable). */
static void index_termlist(const uint8_t* start, const uint8_t* end, struct aml_node* parent, int depth) {
    if (depth > AML_MAX_DEPTH) return;
    const uint8_t* p = start;
    while (p < end) {
        uint8_t op = *p;

        if (op == 0x08) {                                    /* NameOp */
            p++;
            char path[NAME_BUF_SZ]; parse_name_string(&p, path, sizeof(path));
            struct aml_node* n = create_or_get_path(parent, path);
            struct aml_frame f; f.scope = parent;
            for (int i = 0; i < 8; i++) f.locals[i] = mkint(0);
            for (int i = 0; i < 7; i++) f.args[i] = mkint(0);
            struct eval_result v = eval_term(&p, end, &f);
            if (v.status != EXEC_NORMAL) return;
            if (n) { n->kind = NODE_NAME; n->value = v.value; n->value_valid = 1; }
        } else if (op == 0x10) {                              /* ScopeOp */
            p++;
            const uint8_t* pkg_start = p; uint32_t len = parse_pkglength(&p); const uint8_t* body_end = pkg_start + len;
            char path[NAME_BUF_SZ]; parse_name_string(&p, path, sizeof(path));
            struct aml_node* n = create_or_get_path(parent, path);
            index_termlist(p, body_end, n ? n : parent, depth + 1);
            p = body_end;
        } else if (op == 0x14) {                              /* MethodOp */
            p++;
            const uint8_t* pkg_start = p; uint32_t len = parse_pkglength(&p); const uint8_t* body_end = pkg_start + len;
            char path[NAME_BUF_SZ]; parse_name_string(&p, path, sizeof(path));
            uint8_t flags = (p < body_end) ? *p : 0; if (p < body_end) p++;
            struct aml_node* n = create_or_get_path(parent, path);
            if (n) { n->kind = NODE_METHOD; n->method_start = p; n->method_len = (uint32_t)(body_end - p); n->method_argc = (uint8_t)(flags & 0x07); }
            p = body_end;
        } else if (op == 0x06) {                              /* AliasOp - not modeled as a real alias, just consumed */
            p++;
            char a[NAME_BUF_SZ], b[NAME_BUF_SZ];
            parse_name_string(&p, a, sizeof(a));
            parse_name_string(&p, b, sizeof(b));
        } else if (op == 0x15) {                              /* ExternalOp: NameString ObjectType ArgCount */
            p++;
            char a[NAME_BUF_SZ]; parse_name_string(&p, a, sizeof(a));
            if (p < end) p++;
            if (p < end) p++;
        } else if (op == 0x5B && p + 1 < end) {                /* Ext-prefixed opcodes */
            uint8_t op2 = p[1];
            if (op2 == 0x82) {                                 /* DeviceOp */
                p += 2;
                const uint8_t* pkg_start = p; uint32_t len = parse_pkglength(&p); const uint8_t* body_end = pkg_start + len;
                char path[NAME_BUF_SZ]; parse_name_string(&p, path, sizeof(path));
                struct aml_node* n = create_or_get_path(parent, path);
                if (n) n->kind = NODE_DEVICE;
                index_termlist(p, body_end, n ? n : parent, depth + 1);
                p = body_end;
            } else if (op2 == 0x80) {                          /* OpRegionOp */
                p += 2;
                char path[NAME_BUF_SZ]; parse_name_string(&p, path, sizeof(path));
                struct aml_node* n = create_or_get_path(parent, path);
                uint8_t space = (p < end) ? *p : 0; if (p < end) p++;
                struct aml_frame f; f.scope = parent;
                for (int i = 0; i < 8; i++) f.locals[i] = mkint(0);
                for (int i = 0; i < 7; i++) f.args[i] = mkint(0);
                struct eval_result off = eval_term(&p, end, &f);
                if (off.status != EXEC_NORMAL) return;
                struct eval_result len_ = eval_term(&p, end, &f);
                if (n) { n->kind = NODE_OPREGION; n->region_space = space; n->region_offset = off.value.integer; }
                if (len_.status != EXEC_NORMAL) return;
            } else if (op2 == 0x81 || op2 == 0x86) {           /* Field / IndexField */
                p += 2;
                const uint8_t* pkg_start = p; uint32_t len = parse_pkglength(&p); const uint8_t* body_end = pkg_start + len;
                char regpath[NAME_BUF_SZ]; parse_name_string(&p, regpath, sizeof(regpath));
                struct aml_node* region = resolve_name(parent, regpath);
                if (op2 == 0x86) {
                    /* IndexField's index/data register pair isn't
                     * modeled (no indirection support) - the fields
                     * still get named, they'll just read back 0xFF. */
                    char datapath[NAME_BUF_SZ]; parse_name_string(&p, datapath, sizeof(datapath));
                    region = 0;
                }
                if (p < body_end) p++; /* FieldFlags */
                uint32_t bitoff = 0;
                while (p < body_end) {
                    uint8_t b = *p;
                    if (b == 0x00) {                            /* ReservedField: 0x00 PkgLength(bits) */
                        p++;
                        uint32_t w = parse_pkglength(&p);
                        bitoff += w;
                    } else if (b == 0x01) {                     /* AccessField: 0x01 AccessType AccessAttrib */
                        p++; if (p < body_end) p++; if (p < body_end) p++;
                    } else if (b == 0x03) {                     /* ExtendedAccessField: 3 more bytes */
                        p++; p += 3;
                    } else if (b == 0x02) {                     /* ConnectField - not modeled, stop this list */
                        p = body_end;
                    } else if (is_lead_name_char(b)) {          /* NamedField: NameSeg PkgLength(bits) */
                        char seg[5]; for (int i = 0; i < 4; i++) seg[i] = (char)p[i]; seg[4] = 0;
                        p += 4;
                        uint32_t w = parse_pkglength(&p);
                        struct aml_node* fn = create_or_get_path(parent, seg);
                        if (fn) { fn->kind = NODE_FIELDUNIT; fn->region = region; fn->bit_offset = bitoff; fn->bit_len = w; }
                        bitoff += w;
                    } else {
                        p = body_end; /* unrecognized field-list byte - bail out defensively */
                    }
                }
                p = body_end;
            } else if (op2 == 0x83) {                          /* ProcessorOp */
                p += 2;
                const uint8_t* pkg_start = p; uint32_t len = parse_pkglength(&p); const uint8_t* body_end = pkg_start + len;
                char path[NAME_BUF_SZ]; parse_name_string(&p, path, sizeof(path));
                p = (body_end - p >= 6) ? p + 6 : body_end;
                struct aml_node* n = create_or_get_path(parent, path);
                index_termlist(p, body_end, n ? n : parent, depth + 1);
                p = body_end;
            } else if (op2 == 0x84) {                          /* PowerResourceOp */
                p += 2;
                const uint8_t* pkg_start = p; uint32_t len = parse_pkglength(&p); const uint8_t* body_end = pkg_start + len;
                char path[NAME_BUF_SZ]; parse_name_string(&p, path, sizeof(path));
                p = (body_end - p >= 3) ? p + 3 : body_end;
                struct aml_node* n = create_or_get_path(parent, path);
                index_termlist(p, body_end, n ? n : parent, depth + 1);
                p = body_end;
            } else if (op2 == 0x85) {                          /* ThermalZoneOp */
                p += 2;
                const uint8_t* pkg_start = p; uint32_t len = parse_pkglength(&p); const uint8_t* body_end = pkg_start + len;
                char path[NAME_BUF_SZ]; parse_name_string(&p, path, sizeof(path));
                struct aml_node* n = create_or_get_path(parent, path);
                index_termlist(p, body_end, n ? n : parent, depth + 1);
                p = body_end;
            } else {
                /* Mutex/Event/etc - not modeled as namespace objects;
                 * best-effort skip via the general evaluator. This is
                 * also where If/Else/While land (they're not handled
                 * as namespace-defining ops above), and their
                 * predicates routinely call _OSI(), which isn't a real
                 * named object and so fails to resolve. A failed
                 * predicate still correctly advances the cursor past
                 * the whole block (see eval_term's IfOp/WhileOp
                 * handling), so a non-EXEC_NORMAL status here does NOT
                 * mean indexing lost its place - only stop if the
                 * cursor truly didn't move, or every sibling after the
                 * first If(_OSI(...)) in a scope (extremely common on
                 * real hardware) would silently disappear from the
                 * namespace, taking any devices declared after it
                 * (e.g. a battery) with it. */
                struct aml_frame f; f.scope = parent;
                for (int i = 0; i < 8; i++) f.locals[i] = mkint(0);
                for (int i = 0; i < 7; i++) f.args[i] = mkint(0);
                const uint8_t* before = p;
                eval_term(&p, end, &f);
                if (p == before) return;
            }
        } else {
            /* Anything else legally shouldn't appear directly in an
             * ObjectList, but be lenient: try the general evaluator so
             * the cursor still advances; bail only if it truly can't
             * (see the comment above for why a failure status alone
             * isn't a reason to give up on the rest of this scope). */
            struct aml_frame f; f.scope = parent;
            for (int i = 0; i < 8; i++) f.locals[i] = mkint(0);
            for (int i = 0; i < 7; i++) f.args[i] = mkint(0);
            const uint8_t* before = p;
            eval_term(&p, end, &f);
            if (p == before) return;
        }
    }
}

/* ---- _HID / _CID matching ------------------------------------------------- */

static int eisa_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}
static uint32_t eisa_encode(const char* id) {
    uint32_t v =
        ((uint32_t)(id[0] - 0x40) << 26) |
        ((uint32_t)(id[1] - 0x40) << 21) |
        ((uint32_t)(id[2] - 0x40) << 16) |
        ((uint32_t)eisa_hexval(id[3]) << 12) |
        ((uint32_t)eisa_hexval(id[4]) << 8) |
        ((uint32_t)eisa_hexval(id[5]) << 4) |
        ((uint32_t)eisa_hexval(id[6]));
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v & 0xFF000000) >> 24);
}
static int hid_value_matches(struct aml_value* v, const char* hid) {
    if (v->type == AML_STRING) {
        uint32_t n = 0; while (hid[n]) n++;
        if (v->str_len != n) return 0;
        for (uint32_t i = 0; i < n; i++) if (v->str[i] != hid[i]) return 0;
        return 1;
    }
    if (v->type == AML_INTEGER) return v->integer == eisa_encode(hid);
    return 0;
}

/* ---- public API ------------------------------------------------------------ */

void aml_reset_namespace(void) {
    g_node_count = 0;
    g_pkg_pool_used = 0;
    g_static_pkg_pool_used = 0;
    g_call_depth = 0;
    g_indexing_mode = 0;
    struct aml_node* root = node_alloc();
    if (root) { root->parent = 0; root->kind = NODE_ROOT; }
    g_root = root;
}

void aml_index_table(struct acpi_sdt_header* table) {
    if (!table || !g_root) return;
    const uint8_t* body = (const uint8_t*)table + sizeof(struct acpi_sdt_header);
    const uint8_t* end = (const uint8_t*)table + table->length;
    g_indexing_mode = 1;
    index_termlist(body, end, g_root, 0);
    g_indexing_mode = 0;
}

int aml_find_devices_by_hid(const char* hid, struct aml_node** out, int max_out) {
    int found = 0;
    for (int i = 0; i < g_node_count && found < max_out; i++) {
        struct aml_node* n = &g_nodes[i];
        if (n->kind != NODE_DEVICE) continue;
        struct aml_node* hid_node = find_named_child(n, "_HID");
        struct aml_node* cid_node = find_named_child(n, "_CID");
        int matched = 0;
        if (hid_node) {
            struct aml_value v;
            if (aml_evaluate(hid_node, &v) && hid_value_matches(&v, hid)) matched = 1;
        }
        if (!matched && cid_node) {
            struct aml_value v;
            if (aml_evaluate(cid_node, &v)) {
                if (v.type == AML_PACKAGE) {
                    for (uint32_t k = 0; k < v.pkg_count && !matched; k++)
                        if (hid_value_matches(&v.pkg_elems[k], hid)) matched = 1;
                } else if (hid_value_matches(&v, hid)) matched = 1;
            }
        }
        if (matched) out[found++] = n;
    }
    return found;
}

struct aml_node* aml_child(struct aml_node* device, const char* name4) {
    if (!device) return 0;
    return find_named_child(device, name4);
}

int aml_node_count(void) {
    return g_node_count;
}

int aml_node_capacity(void) {
    return AML_MAX_NODES;
}

int aml_device_count(void) {
    int n = 0;
    for (int i = 0; i < g_node_count; i++) {
        if (g_nodes[i].kind == NODE_DEVICE) n++;
    }
    return n;
}

int aml_evaluate(struct aml_node* node, struct aml_value* out) {
    if (!node || !out) return 0;
    g_pkg_pool_used = 0;
    g_call_depth = 0;
    struct aml_value v;
    if (node->kind == NODE_METHOD) v = call_method(node, 0, 0);
    else v = node_read_value(node);
    if (v.type == AML_UNINITIALIZED) return 0;
    *out = v;
    return 1;
}
