#include "smbios.h"
#include "vga.h"
#include <stdint.h>
#include <stddef.h>

struct smbios_header {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
} __attribute__((packed));

static uint8_t* g_table_addr = 0;
static uint32_t g_table_len = 0;
static int g_found = 0;

static int mem_eq(const void* a, const void* b, size_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) if (pa[i] != pb[i]) return 0;
    return 1;
}

/* Return pointer to the Nth (1-indexed) string following a formatted structure. */
static const char* smbios_get_string(struct smbios_header* hdr, uint8_t index) {
    if (index == 0) return "(none)";
    const char* str = (const char*)hdr + hdr->length;
    uint8_t cur = 1;
    while (cur < index) {
        while (*str != '\0') str++;
        str++;
        if (*str == '\0') return "(none)"; /* ran off the end */
        cur++;
    }
    if (*str == '\0') return "(none)";
    return str;
}

static void find_entry_point(void) {
    uint8_t* p = (uint8_t*)0xF0000;
    uint8_t* end = (uint8_t*)0xFFFF0;
    while (p < end) {
        if (mem_eq(p, "_SM_", 4)) {
            uint16_t tlen  = *(uint16_t*)(p + 0x16);
            uint32_t taddr = *(uint32_t*)(p + 0x18);
            g_table_addr = (uint8_t*)(uintptr_t)taddr;
            g_table_len = tlen;
            g_found = 1;
            return;
        }
        if (mem_eq(p, "_SM3_", 5)) {
            uint32_t tlen = *(uint32_t*)(p + 0x0C);
            uint64_t taddr = *(uint64_t*)(p + 0x10);
            g_table_addr = (uint8_t*)(uintptr_t)taddr;
            g_table_len = tlen;
            g_found = 1;
            return;
        }
        p += 16;
    }
}

typedef void (*struct_cb)(struct smbios_header* hdr);

static void smbios_walk(struct_cb cb) {
    if (!g_found) return;
    uint8_t* p = g_table_addr;
    uint8_t* end = g_table_addr + g_table_len;
    while (p < end) {
        struct smbios_header* hdr = (struct smbios_header*)p;
        if (hdr->type == 127) break; /* end-of-table marker */
        if (hdr->length < 4) break;  /* malformed, bail out */
        cb(hdr);
        /* skip formatted area, then find double-null terminator of string set */
        uint8_t* s = p + hdr->length;
        if (s[0] == 0 && s[1] == 0) {
            p = s + 2;
        } else {
            while (!(s[0] == 0 && s[1] == 0)) s++;
            p = s + 2;
        }
    }
}

static void print_field(const char* label, struct smbios_header* hdr, uint8_t off) {
    terminal_writestring(label);
    if (off < hdr->length) {
        uint8_t idx = *((uint8_t*)hdr + off);
        terminal_writestring(smbios_get_string(hdr, idx));
    } else {
        terminal_writestring("(n/a)");
    }
    terminal_putchar('\n');
}

static void cb_bios(struct smbios_header* hdr) {
    if (hdr->type != 0) return;
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_writestring("== BIOS/Firmware ==\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    print_field("  Vendor:   ", hdr, 0x04);
    print_field("  Version:  ", hdr, 0x05);
    print_field("  Release:  ", hdr, 0x08);
}

static void cb_system(struct smbios_header* hdr) {
    if (hdr->type != 1) return;
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_writestring("== System ==\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    print_field("  Manufacturer: ", hdr, 0x04);
    print_field("  Product Name: ", hdr, 0x05);
    print_field("  Version:      ", hdr, 0x06);
    print_field("  Serial Number:", hdr, 0x07);
    if (hdr->length >= 0x1B) {
        print_field("  SKU:          ", hdr, 0x19);
        print_field("  Family:       ", hdr, 0x1A);
    }
}

static void cb_baseboard(struct smbios_header* hdr) {
    if (hdr->type != 2) return;
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_writestring("== Baseboard ==\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    print_field("  Manufacturer: ", hdr, 0x04);
    print_field("  Product:      ", hdr, 0x05);
    print_field("  Version:      ", hdr, 0x06);
    print_field("  Serial Number:", hdr, 0x07);
}

static void cb_processor(struct smbios_header* hdr) {
    if (hdr->type != 4) return;
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_writestring("== Processor (SMBIOS) ==\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
    print_field("  Socket:       ", hdr, 0x04);
    print_field("  Manufacturer: ", hdr, 0x07);
    print_field("  Version:      ", hdr, 0x10);
}

void smbios_print(void) {
    find_entry_point();
    if (!g_found) {
        terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
        terminal_writestring("== SMBIOS/DMI ==\n");
        terminal_setcolor(VGA_LIGHT_RED, VGA_BLACK);
        terminal_writestring("  No SMBIOS entry point found (0xF0000-0xFFFFF).\n");
        terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    smbios_walk(cb_bios);
    smbios_walk(cb_system);
    smbios_walk(cb_baseboard);
    smbios_walk(cb_processor);
}
