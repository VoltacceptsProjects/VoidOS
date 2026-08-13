#include "battery.h"
#include "acpi.h"
#include "aml.h"
#include "vga.h"
#include <stdint.h>

/* ---- ACPI battery constants (ACPI spec 10.2, Control-Method Batteries) --
 *
 * _BIF (legacy, 13-element package) and _BIX (ACPI 4.0+, 20-element
 * package, superset of _BIF with a leading Revision and a Cycle Count)
 * both describe the battery's identity/design numbers. _BST (4-element
 * package) is the live snapshot. Any 32-bit field can come back as
 * 0xFFFFFFFF, which the spec defines as "unknown/not supported". */

#define MAX_BATTERIES        4
#define BATTERY_UNIT_MAH     1          /* _BIF/_BIX element 0/1 == 1 -> capacities are in mA/mAh */
#define BST_DISCHARGING      0x1u
#define BST_CHARGING         0x2u
#define BST_CRITICAL         0x4u
#define FIELD_UNKNOWN_32     0xFFFFFFFFu

struct battery_info {
    int have_info;
    uint32_t power_unit;
    uint32_t design_cap;
    uint32_t full_cap;
    uint32_t design_voltage;
    uint32_t technology;
    uint32_t cycle_count;    /* only from _BIX; FIELD_UNKNOWN_32 if from _BIF */
    const char* model;
    const char* serial;
    const char* type;
};

static uint64_t pkg_int(const struct aml_value* pkg, uint32_t idx) {
    if (idx >= pkg->pkg_count) return FIELD_UNKNOWN_32;
    const struct aml_value* v = &pkg->pkg_elems[idx];
    if (v->type != AML_INTEGER) return FIELD_UNKNOWN_32;
    return v->integer;
}

/* Copies at most scratch_sz-1 bytes of an AML_STRING package element
 * into scratch and NUL-terminates it. AML_PACKAGE contents only live
 * until the next aml_evaluate() call, so anything printed later than
 * that must already have been copied out - this is that copy. */
static const char* pkg_string(const struct aml_value* pkg, uint32_t idx, char* scratch, uint32_t scratch_sz) {
    if (idx >= pkg->pkg_count) return "(n/a)";
    const struct aml_value* v = &pkg->pkg_elems[idx];
    if (v->type != AML_STRING) return "(n/a)";
    uint32_t n = v->str_len;
    if (n >= scratch_sz) n = scratch_sz - 1;
    for (uint32_t i = 0; i < n; i++) scratch[i] = v->str[i];
    scratch[n] = '\0';
    return (n == 0) ? "(none)" : scratch;
}

static void print_uint_field(const char* label, uint32_t v, const char* suffix) {
    terminal_writestring(label);
    if (v == FIELD_UNKNOWN_32) {
        terminal_writestring("unknown");
    } else {
        terminal_write_uint(v);
        if (suffix) terminal_writestring(suffix);
    }
    terminal_putchar('\n');
}

/* num/den as a whole-number percentage, clamped to [0, 100] - batteries
 * routinely report a "remaining" or "full charge" a hair over 100% of
 * the reference capacity as they age or right after a firmware
 * recalibration, which would otherwise print a confusing 101%+. */
static void print_percent(uint64_t num, uint64_t den) {
    if (den == 0) { terminal_writestring("n/a"); return; }
    uint64_t pct = (num * 100) / den;
    if (pct > 100) pct = 100;
    terminal_write_uint((uint32_t)pct);
    terminal_putchar('%');
}

/* Reads _BIX if present (preferred - newer and a superset of _BIF),
 * otherwise falls back to _BIF. string_scratch[3] backs model/serial/
 * type, since the package they came from is only valid until the next
 * aml_evaluate() call this function makes internally. */
static struct battery_info read_battery_info(struct aml_node* dev, char string_scratch[3][32]) {
    struct battery_info info;
    info.have_info = 0;
    info.power_unit = 0;
    info.design_cap = FIELD_UNKNOWN_32;
    info.full_cap = FIELD_UNKNOWN_32;
    info.design_voltage = FIELD_UNKNOWN_32;
    info.technology = FIELD_UNKNOWN_32;
    info.cycle_count = FIELD_UNKNOWN_32;
    info.model = "(n/a)";
    info.serial = "(n/a)";
    info.type = "(n/a)";

    struct aml_node* bix = aml_child(dev, "_BIX");
    struct aml_value v;

    if (bix && aml_evaluate(bix, &v) && v.type == AML_PACKAGE && v.pkg_count >= 20) {
        info.have_info = 1;
        info.power_unit     = (uint32_t)pkg_int(&v, 1);
        info.design_cap     = (uint32_t)pkg_int(&v, 2);
        info.full_cap       = (uint32_t)pkg_int(&v, 3);
        info.technology     = (uint32_t)pkg_int(&v, 4);
        info.design_voltage = (uint32_t)pkg_int(&v, 5);
        info.cycle_count    = (uint32_t)pkg_int(&v, 8);
        info.model  = pkg_string(&v, 16, string_scratch[0], 32);
        info.serial = pkg_string(&v, 17, string_scratch[1], 32);
        info.type   = pkg_string(&v, 18, string_scratch[2], 32);
        return info;
    }

    struct aml_node* bif = aml_child(dev, "_BIF");
    if (bif && aml_evaluate(bif, &v) && v.type == AML_PACKAGE && v.pkg_count >= 13) {
        info.have_info = 1;
        info.power_unit     = (uint32_t)pkg_int(&v, 0);
        info.design_cap     = (uint32_t)pkg_int(&v, 1);
        info.full_cap       = (uint32_t)pkg_int(&v, 2);
        info.technology     = (uint32_t)pkg_int(&v, 3);
        info.design_voltage = (uint32_t)pkg_int(&v, 4);
        info.model  = pkg_string(&v, 9, string_scratch[0], 32);
        info.serial = pkg_string(&v, 10, string_scratch[1], 32);
        info.type   = pkg_string(&v, 11, string_scratch[2], 32);
    }
    return info;
}

/* _STA bit 0 = device present at all, bit 4 = battery actually inserted
 * (bit 4 is battery-specific; ACPI 6.x sec 6.3.7). A DSDT that omits
 * _STA entirely is treated as "always present", which is common for
 * simple/fixed (non-swappable) battery AML. Returns 0 (not present,
 * caller should stop), 1 (slot present but empty, caller should
 * stop), or 2 (present with a battery installed - proceed). */
static int check_presence(struct aml_node* dev) {
    struct aml_node* sta = aml_child(dev, "_STA");
    if (!sta) return 2;
    struct aml_value v;
    if (!aml_evaluate(sta, &v) || v.type != AML_INTEGER) return 2;
    if (!(v.integer & 0x1)) return 0;
    if (!(v.integer & 0x10)) return 1;
    return 2;
}

static void print_one_battery(struct aml_node* dev, int index, int total) {
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    if (total > 1) {
        terminal_writestring("== Battery ");
        terminal_write_uint((uint32_t)(index + 1));
        terminal_writestring(" ==\n");
    } else {
        terminal_writestring("== Battery ==\n");
    }
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    int presence = check_presence(dev);
    if (presence == 0) {
        terminal_writestring("  Not present.\n");
        return;
    }
    if (presence == 1) {
        terminal_writestring("  Battery bay present, no battery installed.\n");
        return;
    }

    char string_scratch[3][32];
    struct battery_info info = read_battery_info(dev, string_scratch);
    const char* cap_unit = (info.power_unit == BATTERY_UNIT_MAH) ? "mAh" : "mWh";
    const char* rate_unit = (info.power_unit == BATTERY_UNIT_MAH) ? "mA" : "mW";

    if (info.have_info) {
        terminal_writestring("  Model:                "); terminal_writestring(info.model);  terminal_putchar('\n');
        terminal_writestring("  Serial number:        "); terminal_writestring(info.serial); terminal_putchar('\n');
        terminal_writestring("  Type:                 "); terminal_writestring(info.type);   terminal_putchar('\n');
        terminal_writestring("  Chemistry:            ");
        terminal_writestring(info.technology == 1 ? "Rechargeable" :
                              info.technology == 0 ? "Non-rechargeable" : "unknown");
        terminal_putchar('\n');

        print_uint_field("  Design capacity:      ", info.design_cap, cap_unit);
        print_uint_field("  Full charge capacity: ", info.full_cap, cap_unit);
        print_uint_field("  Design voltage:       ", info.design_voltage, " mV");
        if (info.cycle_count != FIELD_UNKNOWN_32) {
            print_uint_field("  Cycle count:          ", info.cycle_count, 0);
        }

        terminal_writestring("  Health:               ");
        if (info.design_cap != FIELD_UNKNOWN_32 && info.design_cap != 0 && info.full_cap != FIELD_UNKNOWN_32) {
            print_percent(info.full_cap, info.design_cap);
            terminal_writestring(" of design capacity");
        } else {
            terminal_writestring("n/a");
        }
        terminal_putchar('\n');
    } else {
        terminal_writestring("  No _BIF/_BIX method (or evaluation failed) - identity/health unavailable.\n");
    }

    struct aml_node* bst = aml_child(dev, "_BST");
    if (!bst) {
        terminal_writestring("  No _BST method found.\n");
        return;
    }
    struct aml_value st;
    if (!aml_evaluate(bst, &st) || st.type != AML_PACKAGE || st.pkg_count < 4) {
        terminal_writestring("  _BST evaluation failed.\n");
        return;
    }

    uint32_t state     = (uint32_t)pkg_int(&st, 0);
    uint32_t rate       = (uint32_t)pkg_int(&st, 1);
    uint32_t remaining = (uint32_t)pkg_int(&st, 2);
    uint32_t voltage   = (uint32_t)pkg_int(&st, 3);

    terminal_writestring("  Status:               ");
    if (state & BST_CRITICAL) terminal_writestring("CRITICAL, ");
    if (state & BST_CHARGING) terminal_writestring("Charging");
    else if (state & BST_DISCHARGING) terminal_writestring("Discharging");
    else terminal_writestring("On AC / not charging");
    terminal_putchar('\n');

    print_uint_field("  Remaining capacity:   ", remaining, cap_unit);
    print_uint_field("  Present rate:         ", rate, rate_unit);
    print_uint_field("  Present voltage:      ", voltage, " mV");

    terminal_writestring("  Charge:               ");
    uint32_t denom = (info.full_cap != FIELD_UNKNOWN_32 && info.full_cap != 0) ? info.full_cap
                    : (info.design_cap != FIELD_UNKNOWN_32 ? info.design_cap : 0);
    if (remaining != FIELD_UNKNOWN_32 && denom != 0) {
        print_percent(remaining, denom);
    } else {
        terminal_writestring("n/a");
    }
    terminal_putchar('\n');
}

void battery_print(void) {
    acpi_init();
    if (!acpi_available()) {
        terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
        terminal_writestring("== Battery ==\n");
        terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        terminal_writestring("  ACPI not available (no checksummed RSDP/FADT found).\n");
        return;
    }

    aml_reset_namespace();
    struct acpi_sdt_header* dsdt = acpi_get_dsdt();
    if (dsdt) aml_index_table(dsdt);
    for (struct acpi_sdt_header* ssdt = acpi_next_ssdt(0); ssdt; ssdt = acpi_next_ssdt(ssdt)) {
        aml_index_table(ssdt);
    }

    struct aml_node* batteries[MAX_BATTERIES];
    int count = aml_find_devices_by_hid("PNP0C0A", batteries, MAX_BATTERIES);

    if (count == 0) {
        terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
        terminal_writestring("== Battery ==\n");
        terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        terminal_writestring("  No ACPI Control Method Battery (PNP0C0A) found.\n");
        terminal_writestring("  (Desktops and most default VM configurations have none.)\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        if (i > 0) terminal_putchar('\n');
        print_one_battery(batteries[i], i, count);
    }
}
