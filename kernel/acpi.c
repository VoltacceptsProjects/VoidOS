#include "acpi.h"
#include <stddef.h>

/* ---- small local helpers (freestanding - no libc) --------------------- */

static int mem_eq(const void* a, const void* b, size_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

static uint8_t checksum8(const void* p, size_t len) {
    const uint8_t* b = (const uint8_t*)p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum;
}

/* ---- RSDP -------------------------------------------------------------- */

struct acpi_rsdp_v1 {
    char     signature[8]; /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
} __attribute__((packed));

struct acpi_rsdp_v2 {
    struct acpi_rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

/* ---- FADT (only the fields we need, up to X_DSDT at byte offset 140) -- */

struct acpi_fadt {
    struct acpi_sdt_header header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved0;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t boot_arch_flags;
    uint8_t  reserved1;
    uint32_t flags;
    uint8_t  reset_reg[12];
    uint8_t  reset_value;
    uint8_t  reserved2[3];
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
} __attribute__((packed));

static struct acpi_rsdp_v2* g_rsdp = 0;
static uint8_t*  g_root_table = 0;   /* points at RSDT or XSDT body (past header) */
static uint32_t  g_root_entries = 0;
static int       g_root_is_xsdt = 0;
static struct acpi_fadt* g_fadt = 0;
static int g_available = 0;
static int g_inited = 0;

static struct acpi_rsdp_v2* scan_for_rsdp_range(uint8_t* start, uint8_t* end) {
    for (uint8_t* p = start; p + 8 <= end; p += 16) {
        if (mem_eq(p, "RSD PTR ", 8)) {
            struct acpi_rsdp_v1* v1 = (struct acpi_rsdp_v1*)p;
            if (checksum8(v1, sizeof(*v1)) != 0) continue;
            if (v1->revision >= 2) {
                struct acpi_rsdp_v2* v2 = (struct acpi_rsdp_v2*)p;
                if (checksum8(v2, v2->length) != 0) continue;
                return v2;
            }
            /* ACPI 1.0 RSDP - wrap it so callers have one type to deal
             * with. xsdt_address stays zero, which acpi_init() reads
             * as "no XSDT, use the RSDT instead". */
            return (struct acpi_rsdp_v2*)v1;
        }
    }
    return 0;
}

static struct acpi_rsdp_v2* find_rsdp(void) {
    /* Extended BIOS Data Area segment lives as a 16-bit real-mode
     * segment at 0x40E; the RSDP, if in the EBDA, is in its first 1KB.
     * No paging is active here (flat physical == virtual), so these
     * are ordinary pointers. */
    uint16_t ebda_seg = *(volatile uint16_t*)(uintptr_t)0x40E;
    if (ebda_seg) {
        uint8_t* ebda = (uint8_t*)(uintptr_t)((uint32_t)ebda_seg << 4);
        struct acpi_rsdp_v2* found = scan_for_rsdp_range(ebda, ebda + 1024);
        if (found) return found;
    }
    /* Fall back to the main BIOS read-only memory area. */
    return scan_for_rsdp_range((uint8_t*)(uintptr_t)0xE0000, (uint8_t*)(uintptr_t)0x100000);
}

static struct acpi_sdt_header* validated_table(uint32_t phys_addr) {
    if (!phys_addr) return 0;
    struct acpi_sdt_header* hdr = (struct acpi_sdt_header*)(uintptr_t)phys_addr;
    if (checksum8(hdr, hdr->length) != 0) return 0;
    return hdr;
}

static struct acpi_sdt_header* validated_table64(uint64_t phys_addr) {
    if (phys_addr == 0 || phys_addr > 0xFFFFFFFFULL) return 0;
    return validated_table((uint32_t)phys_addr);
}

void acpi_init(void) {
    if (g_inited) return;
    g_inited = 1;

    g_rsdp = find_rsdp();
    if (!g_rsdp) return;

    if (g_rsdp->v1.revision >= 2 && g_rsdp->xsdt_address) {
        struct acpi_sdt_header* xsdt = validated_table64(g_rsdp->xsdt_address);
        if (xsdt) {
            g_root_table = (uint8_t*)xsdt + sizeof(struct acpi_sdt_header);
            g_root_entries = (xsdt->length - (uint32_t)sizeof(struct acpi_sdt_header)) / 8;
            g_root_is_xsdt = 1;
        }
    }
    if (!g_root_table) {
        struct acpi_sdt_header* rsdt = validated_table(g_rsdp->v1.rsdt_address);
        if (!rsdt) return;
        g_root_table = (uint8_t*)rsdt + sizeof(struct acpi_sdt_header);
        g_root_entries = (rsdt->length - (uint32_t)sizeof(struct acpi_sdt_header)) / 4;
        g_root_is_xsdt = 0;
    }

    g_fadt = (struct acpi_fadt*)acpi_find_table("FACP");
    g_available = (g_fadt != 0);
}

int acpi_available(void) {
    return g_available;
}

static struct acpi_sdt_header* root_entry(uint32_t index) {
    if (index >= g_root_entries) return 0;
    if (g_root_is_xsdt) {
        uint64_t addr;
        __builtin_memcpy(&addr, g_root_table + index * 8, 8);
        return validated_table64(addr);
    } else {
        uint32_t addr;
        __builtin_memcpy(&addr, g_root_table + index * 4, 4);
        return validated_table(addr);
    }
}

struct acpi_sdt_header* acpi_find_table(const char* signature) {
    if (!g_rsdp || !g_root_table) return 0;
    for (uint32_t i = 0; i < g_root_entries; i++) {
        struct acpi_sdt_header* t = root_entry(i);
        if (t && mem_eq(t->signature, signature, 4)) return t;
    }
    return 0;
}

struct acpi_sdt_header* acpi_get_dsdt(void) {
    if (!g_available || !g_fadt) return 0;
    struct acpi_sdt_header* dsdt = 0;
    if (g_fadt->header.length >= 148 && g_fadt->x_dsdt) {
        dsdt = validated_table64(g_fadt->x_dsdt);
    }
    if (!dsdt && g_fadt->dsdt) {
        dsdt = validated_table(g_fadt->dsdt);
    }
    return dsdt;
}

struct acpi_sdt_header* acpi_next_ssdt(struct acpi_sdt_header* prev) {
    if (!g_rsdp || !g_root_table) return 0;
    int found_prev = (prev == 0);
    for (uint32_t i = 0; i < g_root_entries; i++) {
        struct acpi_sdt_header* t = root_entry(i);
        if (!t) continue;
        if (!found_prev) {
            if (t == prev) found_prev = 1;
            continue;
        }
        if (mem_eq(t->signature, "SSDT", 4)) return t;
    }
    return 0;
}
