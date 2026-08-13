#include "vga.h"
#include "ui.h"
#include "multiboot.h"
#include "cpuinfo.h"
#include "smbios.h"
#include "pci.h"
#include "meminfo.h"
#include "battery.h"
#include "apps.h"
#include "fs.h"
#include "keyboard.h"
#include "mouse.h"
#include "ps2.h"
#include "io.h"
#include "serial.h"
#include "iwlwifi.h"
#include "appstore.h"
#include "net.h"
#include <stdint.h>

/* Stage 3: if the 9260 is present and its firmware was baked onto the
 * ISO as a Multiboot module (see grub.cfg / Makefile), parse the
 * firmware image and attempt the first step of NIC bring-up. Every
 * bit of this logs to serial (-serial stdio under QEMU) rather than
 * the screen, same as the Stage 2 PCI scan - see kernel/iwlwifi.h for
 * exactly what this does and doesn't do yet. */
static void iwlwifi_stage3(struct multiboot_info* mbi, int found_9260) {
    if (!found_9260) return;

    const uint8_t* fw_data;
    uint32_t fw_size;
    if (!iwlwifi_find_firmware_module(mbi, &fw_data, &fw_size)) {
        serial_writestring("[iwlwifi] 9260 present but no .ucode module found on "
                            "the ISO - add it to grub.cfg/Makefile like the .vapp "
                            "modules\n");
        return;
    }

    struct iwlwifi_fw_image fw;
    if (!iwlwifi_parse_firmware(fw_data, fw_size, &fw)) return;

    uint32_t bar0 = pci_get_9260_bar0();
    if (iwlwifi_bringup(bar0)) {
        iwlwifi_load_firmware(bar0, &fw);
    }
}

/* There's no ACPI table parser in this kernel, so we can't walk to the
 * real \_S5 sleep-type value the "proper" way. Instead we hit the fixed
 * ACPI PM1a_CNT-equivalent ports the common emulators hardwire for a
 * "SLP_TYP=5, SLP_EN=1" shutdown regardless of what the guest's ACPI
 * tables say. Covers QEMU (both the modern PIIX4/ICH9 default and the
 * older Bochs-era port) and VirtualBox. On real hardware, or any
 * emulator that doesn't implement one of these, none of this has any
 * effect and we fall through to a plain halt. */
static void power_off(void) {
    outw(0x604, 0x2000);   /* QEMU - PIIX4/ICH9 ACPI PM1a_CNT */
    outw(0xB004, 0x2000);  /* older QEMU / Bochs */
    outw(0x4004, 0x3400);  /* VirtualBox */

    __asm__ volatile ("cli");
    for (;;) { __asm__ volatile ("hlt"); }
}

#define NUM_SECTIONS 8
#define APPLICATIONS_SECTION 6
#define FILES_SECTION 7

static const struct ui_section sections[NUM_SECTIONS] = {
    { "Overview", UI_ICON_GRID },
    { "CPU",      UI_ICON_CPU },
    { "Memory",   UI_ICON_MEMORY },
    { "Display",  UI_ICON_DISPLAY },
    { "System",   UI_ICON_SYSTEM },
    { "Battery",  UI_ICON_BATTERY },
    { "Apps",     UI_ICON_APPS },
    { "App Store", UI_ICON_FILES },
};

static size_t section_start[NUM_SECTIONS];
static size_t section_end[NUM_SECTIONS];

static void print_overview(uint32_t magic) {
    terminal_setcolor(VGA_WHITE, VGA_DARK_GREY);
    terminal_writestring("Welcome to VoidOS\n\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    terminal_writestring("VoidOS boots straight into a live snapshot of this\n");
    terminal_writestring("machine's hardware - CPU, memory, display and\n");
    terminal_writestring("system identity - read directly from firmware and\n");
    terminal_writestring("the CPU itself, no OS installation required.\n\n");

    terminal_setcolor(VGA_LIGHT_CYAN, VGA_DARK_GREY);
    terminal_writestring("Getting around\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    terminal_writestring("  Left / Right    switch between sections\n");
    terminal_writestring("  Up / Down       scroll a line at a time\n");
    terminal_writestring("  PgUp / PgDn     scroll a page at a time\n");
    terminal_writestring("  Home / End      jump to top / bottom\n");
    terminal_writestring("  Enter           open the selected section or Apps\n");
    terminal_writestring("  Esc             shut down\n");
    terminal_writestring("  Mouse           click a section in the sidebar\n\n");

    if (magic != 0x2BADB002) {
        terminal_setcolor(VGA_LIGHT_RED, VGA_DARK_GREY);
        terminal_writestring("Warning: not loaded by a Multiboot-compliant\n");
        terminal_writestring("loader - hardware data below may be incomplete.\n\n");
        terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    }

    terminal_setcolor(VGA_DARK_GREY, VGA_DARK_GREY);
    terminal_writestring("Use Right to open the CPU section now, or choose Apps to run a program.\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
}

/* Populates the shared scrollback buffer with every section back to
 * back (same approach the original report used), but records each
 * section's [start, end] line range so the GUI can jump straight to
 * one and keep scrolling confined to it. */
static void generate_content(uint32_t magic, struct multiboot_info* mbi) {
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);

    section_start[0] = terminal_line_count() - 1;
    print_overview(magic);
    section_end[0] = terminal_line_count() - 1;

    section_start[1] = terminal_line_count();
    terminal_putchar('\n');
    cpuinfo_print();
    section_end[1] = terminal_line_count() - 1;

    section_start[2] = terminal_line_count();
    terminal_putchar('\n');
    meminfo_print(mbi);
    section_end[2] = terminal_line_count() - 1;

    section_start[3] = terminal_line_count();
    terminal_putchar('\n');
    pci_print_display_devices();
    section_end[3] = terminal_line_count() - 1;

    section_start[4] = terminal_line_count();
    terminal_putchar('\n');
    smbios_print();
    section_end[4] = terminal_line_count() - 1;

    section_start[5] = terminal_line_count();
    terminal_putchar('\n');
    battery_print();
    section_end[5] = terminal_line_count() - 1;

    section_start[6] = terminal_line_count();
    terminal_putchar('\n');
    apps_print_launcher();
    section_end[6] = terminal_line_count() - 1;

    section_start[7] = terminal_line_count();
    terminal_putchar('\n');
    voidfs_print_files();
    section_end[7] = terminal_line_count() - 1;
}

void kernel_main(uint32_t magic, struct multiboot_info* mbi) {
    serial_init();
    serial_writestring("\n[boot] VoidOS starting, serial debug channel up\n");

    terminal_initialize(mbi);

    /* Carve the text/scrollback grid down into the GUI's content card
     * (no-op in legacy text mode, where there's no room for a sidebar
     * and the report fills the whole screen as before). */
    uint32_t vx, vy, vw, vh;
    ui_content_viewport(&vx, &vy, &vw, &vh);
    terminal_set_viewport(vx, vy, vw, vh);

    ps2_init();
    if (gfx_available()) {
        mouse_set_bounds((int32_t)gfx_screen_width() - 1, (int32_t)gfx_screen_height() - 1);
    }

    voidfs_initialize();
    voidfs_install_multiboot_modules(mbi);
    int found_9260 = pci_probe_network_devices();
    iwlwifi_stage3(mbi, found_9260);
    net_init();
    generate_content(magic, mbi);

    int selected = 0;
    size_t top = section_start[selected];

    ui_draw_shell(sections, NUM_SECTIONS, selected);
    terminal_render_section(top, section_start[selected], section_end[selected]);
    ui_draw_cursor(mouse_get_state()->x, mouse_get_state()->y);

    for (;;) {
        ps2_poll();
        enum key k = keyboard_poll_key();
        const struct mouse_state* ms = mouse_get_state();

        int changed = 0;
        int redraw_shell = 0;

        if (k != KEY_NONE) {
            changed = 1;
            switch (k) {
                case KEY_LEFT:
                    if (selected > 0) {
                        selected--;
                        top = section_start[selected];
                        redraw_shell = 1;
                    }
                    break;
                case KEY_RIGHT:
                    if (selected < NUM_SECTIONS - 1) {
                        selected++;
                        top = section_start[selected];
                        redraw_shell = 1;
                    }
                    break;
                case KEY_ENTER:
                    if (selected == APPLICATIONS_SECTION) {
                        apps_run_launcher();
                        redraw_shell = 1;
                    } else if (selected == FILES_SECTION) {
                        appstore_run(mbi);
                        redraw_shell = 1;
                    } else if (selected < NUM_SECTIONS - 1) {
                        selected++;
                        top = section_start[selected];
                        redraw_shell = 1;
                    }
                    break;
                case KEY_UP:
                    if (top > section_start[selected]) top--;
                    break;
                case KEY_DOWN:
                    top++;
                    break;
                case KEY_PAGE_UP: {
                    size_t rows = terminal_visible_rows();
                    top = (top > section_start[selected] + rows) ? top - rows : section_start[selected];
                    break;
                }
                case KEY_PAGE_DOWN:
                    top += terminal_visible_rows();
                    break;
                case KEY_HOME:
                    top = section_start[selected];
                    break;
                case KEY_END:
                    top = section_end[selected];
                    break;
                case KEY_ESC:
                    power_off();
                    break; /* unreachable */
                case KEY_NONE:
                default:
                    break;
            }
        }

        if (ms->left_clicked) {
            int idx = ui_hit_test_sidebar((uint32_t)ms->x, (uint32_t)ms->y, NUM_SECTIONS);
            if (idx == APPLICATIONS_SECTION) {
                selected = APPLICATIONS_SECTION;
                top = section_start[selected];
                apps_run_launcher();
                redraw_shell = 1;
            } else if (idx == FILES_SECTION) {
                selected = FILES_SECTION;
                top = section_start[selected];
                appstore_run(mbi);
                redraw_shell = 1;
            } else if (idx >= 0 && idx != selected) {
                selected = idx;
                top = section_start[selected];
                redraw_shell = 1;
            }
            changed = 1;
        }
        if (ms->moved) {
            changed = 1; /* ui_draw_cursor() below patches just the cursor-sized rect */
        }

        mouse_clear_events();

        if (!changed) continue;

        /* Only repaint the shell/content when something actually changed
         * underneath them - a plain mouse move touches neither, so it
         * falls straight through to the cursor-only update below instead
         * of forcing a full-screen redraw on every PS/2 packet. */
        if (redraw_shell) {
            ui_draw_shell(sections, NUM_SECTIONS, selected);
            terminal_render_section(top, section_start[selected], section_end[selected]);
            ui_cursor_invalidate(); /* shell repaint already overwrote the old cursor pixels */
        } else if (k != KEY_NONE) {
            terminal_render_section(top, section_start[selected], section_end[selected]);
            ui_cursor_invalidate(); /* content repaint may have overwritten the old cursor pixels */
        }
        ui_draw_cursor(ms->x, ms->y);
    }
}
