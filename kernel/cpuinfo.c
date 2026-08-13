#include "cpuinfo.h"
#include "vga.h"
#include <stdint.h>

extern int cpuid_supported(void);
extern void do_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* regs);

static void print_reg_as_chars(uint32_t reg) {
    char c;
    c = (char)(reg & 0xFF); if (c) terminal_putchar(c);
    c = (char)((reg >> 8) & 0xFF); if (c) terminal_putchar(c);
    c = (char)((reg >> 16) & 0xFF); if (c) terminal_putchar(c);
    c = (char)((reg >> 24) & 0xFF); if (c) terminal_putchar(c);
}

void cpuinfo_print(void) {
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_writestring("== CPU ==\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    if (!cpuid_supported()) {
        terminal_writestring("  CPUID instruction not supported on this CPU.\n");
        return;
    }

    uint32_t regs[4];

    /* Vendor ID string (leaf 0) */
    do_cpuid(0, 0, regs);
    uint32_t max_leaf = regs[0];
    terminal_writestring("  Vendor:   ");
    print_reg_as_chars(regs[1]); /* EBX */
    print_reg_as_chars(regs[3]); /* EDX */
    print_reg_as_chars(regs[2]); /* ECX */
    terminal_putchar('\n');

    /* Family/Model/Stepping (leaf 1) */
    if (max_leaf >= 1) {
        do_cpuid(1, 0, regs);
        uint32_t eax = regs[0];
        uint32_t stepping = eax & 0xF;
        uint32_t model = (eax >> 4) & 0xF;
        uint32_t family = (eax >> 8) & 0xF;
        uint32_t ext_model = (eax >> 16) & 0xF;
        uint32_t ext_family = (eax >> 20) & 0xFF;

        uint32_t disp_family = family;
        uint32_t disp_model = model;
        if (family == 0xF) disp_family = family + ext_family;
        if (family == 0x6 || family == 0xF) disp_model = (ext_model << 4) + model;

        terminal_writestring("  Family:   ");
        terminal_write_uint(disp_family);
        terminal_writestring("   Model: ");
        terminal_write_uint(disp_model);
        terminal_writestring("   Stepping: ");
        terminal_write_uint(stepping);
        terminal_putchar('\n');

        uint32_t ebx = regs[1];
        uint32_t logical_cpus = (ebx >> 16) & 0xFF;
        terminal_writestring("  Logical CPUs (per package, leaf1 EBX): ");
        terminal_write_uint(logical_cpus);
        terminal_putchar('\n');

        uint32_t edx = regs[3];
        uint32_t ecx = regs[2];
        terminal_writestring("  Features: ");
        if (edx & (1 << 0))  terminal_writestring("FPU ");
        if (edx & (1 << 4))  terminal_writestring("TSC ");
        if (edx & (1 << 5))  terminal_writestring("MSR ");
        if (edx & (1 << 6))  terminal_writestring("PAE ");
        if (edx & (1 << 9))  terminal_writestring("APIC ");
        if (edx & (1 << 23)) terminal_writestring("MMX ");
        if (edx & (1 << 25)) terminal_writestring("SSE ");
        if (edx & (1 << 26)) terminal_writestring("SSE2 ");
        if (edx & (1 << 28)) terminal_writestring("HTT ");
        if (ecx & (1 << 0))  terminal_writestring("SSE3 ");
        if (ecx & (1 << 9))  terminal_writestring("SSSE3 ");
        if (ecx & (1 << 19)) terminal_writestring("SSE4.1 ");
        if (ecx & (1 << 20)) terminal_writestring("SSE4.2 ");
        if (ecx & (1 << 28)) terminal_writestring("AVX ");
        if (ecx & (1 << 5))  terminal_writestring("VMX ");
        terminal_putchar('\n');
    }

    /* Extended leaves: brand string */
    do_cpuid(0x80000000, 0, regs);
    uint32_t max_ext = regs[0];
    if (max_ext >= 0x80000004) {
        char brand[49];
        uint32_t* bp = (uint32_t*)brand;
        do_cpuid(0x80000002, 0, regs);
        bp[0] = regs[0]; bp[1] = regs[1]; bp[2] = regs[2]; bp[3] = regs[3];
        do_cpuid(0x80000003, 0, regs);
        bp[4] = regs[0]; bp[5] = regs[1]; bp[6] = regs[2]; bp[7] = regs[3];
        do_cpuid(0x80000004, 0, regs);
        bp[8] = regs[0]; bp[9] = regs[1]; bp[10] = regs[2]; bp[11] = regs[3];
        brand[48] = '\0';

        /* trim leading spaces */
        char* p = brand;
        while (*p == ' ') p++;

        terminal_writestring("  Model:    ");
        terminal_writestring(p);
        terminal_putchar('\n');
    }

    if (max_ext >= 0x80000008) {
        do_cpuid(0x80000008, 0, regs);
        uint32_t phys_bits = regs[0] & 0xFF;
        uint32_t virt_bits = (regs[0] >> 8) & 0xFF;
        terminal_writestring("  Addr bits: ");
        terminal_write_uint(phys_bits);
        terminal_writestring(" phys / ");
        terminal_write_uint(virt_bits);
        terminal_writestring(" virt\n");
    }
}
