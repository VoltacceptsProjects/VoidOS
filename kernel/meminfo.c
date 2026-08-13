#include "meminfo.h"
#include "vga.h"

#define MULTIBOOT_INFO_MEMORY   0x001
#define MULTIBOOT_INFO_MEM_MAP  0x040

void meminfo_print(struct multiboot_info* mbi) {
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_writestring("== RAM ==\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    if (mbi->flags & MULTIBOOT_INFO_MEMORY) {
        uint32_t total_kb = mbi->mem_lower + mbi->mem_upper;
        terminal_writestring("  Lower memory: ");
        terminal_write_uint(mbi->mem_lower);
        terminal_writestring(" KB\n  Upper memory: ");
        terminal_write_uint(mbi->mem_upper);
        terminal_writestring(" KB (");
        terminal_write_uint(mbi->mem_upper / 1024);
        terminal_writestring(" MB)\n  Approx total (below any >4GB regions): ");
        terminal_write_uint(total_kb / 1024);
        terminal_writestring(" MB\n");
    } else {
        terminal_writestring("  Basic memory info not provided by bootloader.\n");
    }

    if (mbi->flags & MULTIBOOT_INFO_MEM_MAP) {
        terminal_writestring("  -- Memory map --\n");
        uint8_t* p = (uint8_t*)(uintptr_t)mbi->mmap_addr;
        uint8_t* end = p + mbi->mmap_length;
        uint64_t total_available = 0;
        int count = 0;
        while (p < end && count < 12) {
            struct multiboot_mmap_entry* e = (struct multiboot_mmap_entry*)p;
            terminal_writestring("   addr=0x");
            terminal_write_hex32((uint32_t)(e->addr & 0xFFFFFFFF));
            terminal_writestring(" len=");
            terminal_write_uint64(e->len / 1024);
            terminal_writestring("KB type=");
            terminal_write_uint(e->type);
            if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                terminal_writestring(" (available)");
                total_available += e->len;
            }
            terminal_putchar('\n');
            p += e->size + 4;
            count++;
        }
        terminal_writestring("  Total available (from map): ");
        terminal_write_uint64(total_available / (1024 * 1024));
        terminal_writestring(" MB\n");
    }
}
