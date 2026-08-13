#include "pci.h"
#include "vga.h"
#include "io.h"
#include "serial.h"
#include <stdint.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((1u << 31) | (bus << 16) | (dev << 11) |
                                   (func << 8) | (offset & 0xFC));
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static uint16_t pci_vendor_id(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint16_t)(pci_config_read32(bus, dev, func, 0x00) & 0xFFFF);
}
static uint16_t pci_device_id(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint16_t)((pci_config_read32(bus, dev, func, 0x00) >> 16) & 0xFFFF);
}
static uint8_t pci_header_type(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)((pci_config_read32(bus, dev, func, 0x0C) >> 16) & 0xFF);
}
static uint8_t pci_class(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)((pci_config_read32(bus, dev, func, 0x08) >> 24) & 0xFF);
}
static uint8_t pci_subclass(uint8_t bus, uint8_t dev, uint8_t func) {
    return (uint8_t)((pci_config_read32(bus, dev, func, 0x08) >> 16) & 0xFF);
}

static const char* vendor_name(uint16_t vid) {
    switch (vid) {
        case 0x8086: return "Intel Corporation";
        case 0x10DE: return "NVIDIA Corporation";
        case 0x1002: return "AMD/ATI";
        case 0x1AF4: return "Red Hat (VirtIO)";
        case 0x1234: return "QEMU/Bochs (std vga)";
        case 0x15AD: return "VMware";
        case 0x80EE: return "VirtualBox (Oracle)";
        case 0x102B: return "Matrox";
        case 0x1013: return "Cirrus Logic";
        case 0x5333: return "S3 Graphics";
        default: return "Unknown vendor";
    }
}

static int found_any = 0;

static void check_device(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t vid = pci_vendor_id(bus, dev, func);
    if (vid == 0xFFFF) return; /* no device present */

    uint8_t cls = pci_class(bus, dev, func);
    uint8_t subcls = pci_subclass(bus, dev, func);

    /* Class 0x03 = Display controller (VGA/XGA/3D/other) */
    if (cls == 0x03) {
        found_any = 1;
        uint16_t did = pci_device_id(bus, dev, func);
        terminal_writestring("  GPU/Display: ");
        terminal_writestring(vendor_name(vid));
        terminal_writestring("  [vendor ");
        terminal_write_hex16(vid);
        terminal_writestring(", device ");
        terminal_write_hex16(did);
        terminal_writestring("]");
        terminal_writestring("  (bus ");
        terminal_write_uint(bus);
        terminal_writestring(" dev ");
        terminal_write_uint(dev);
        terminal_writestring(" func ");
        terminal_write_uint(func);
        terminal_writestring(")\n");

        const char* subtype = "Unknown display type";
        if (subcls == 0x00) subtype = "VGA-compatible controller";
        else if (subcls == 0x01) subtype = "XGA controller";
        else if (subcls == 0x02) subtype = "3D controller";
        else if (subcls == 0x80) subtype = "Other display controller";
        terminal_writestring("    Type: ");
        terminal_writestring(subtype);
        terminal_putchar('\n');
    }
}

void pci_print_display_devices(void) {
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_BLACK);
    terminal_writestring("== GPU / Display (PCI scan) ==\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_BLACK);

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint16_t vid0 = pci_vendor_id((uint8_t)bus, dev, 0);
            if (vid0 == 0xFFFF) continue;

            check_device((uint8_t)bus, dev, 0);

            uint8_t ht = pci_header_type((uint8_t)bus, dev, 0);
            if (ht & 0x80) { /* multi-function device */
                for (uint8_t func = 1; func < 8; func++) {
                    if (pci_vendor_id((uint8_t)bus, dev, func) != 0xFFFF) {
                        check_device((uint8_t)bus, dev, func);
                    }
                }
            }
        }
    }

    if (!found_any) {
        terminal_writestring("  No display controller found via PCI scan.\n");
    }
}

/* PCI header offset 0x10 = BAR0. Bit 0 of the value tells you memory
 * (0) vs I/O (1) space; for a memory BAR the low 4 bits are flags, not
 * part of the address, so they're masked off below. This is the
 * window the 9260's firmware-load and host-command registers live
 * behind - step one of Stage 2 is confirming this address is sane
 * before anything tries to read/write through it. */
static uint32_t pci_bar0(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t raw = pci_config_read32(bus, dev, func, 0x10);
    return raw & 0xFFFFFFF0;
}

static uint32_t g_9260_bar0 = 0;

uint32_t pci_get_9260_bar0(void) {
    return g_9260_bar0;
}

static void probe_net_function(uint16_t bus, uint8_t dev, uint8_t func,
                                int* found_9260, int* found_any_net) {
    uint16_t vid = pci_vendor_id((uint8_t)bus, dev, func);
    if (vid == 0xFFFF) return;
    if (pci_class((uint8_t)bus, dev, func) != 0x02) return;

    *found_any_net = 1;
    uint16_t did = pci_device_id((uint8_t)bus, dev, func);
    uint32_t bar0 = pci_bar0((uint8_t)bus, dev, func);

    serial_writestring("[pci]   net device: vendor=");
    serial_write_hex(vid);
    serial_writestring(" device=");
    serial_write_hex(did);
    serial_writestring(" bus=");
    serial_write_uint(bus);
    serial_writestring(" dev=");
    serial_write_uint(dev);
    serial_writestring(" func=");
    serial_write_uint(func);
    serial_writestring(" bar0=");
    serial_write_hex(bar0);
    serial_writestring("\n");

    if (vid == 0x8086 && did == 0x2526) {
        *found_9260 = 1;
        g_9260_bar0 = bar0;
        serial_writestring("[pci]   ^ this is the Intel Wireless-AC 9260\n");
    }
}

int pci_probe_network_devices(void) {
    serial_writestring("[pci] scanning for network controllers (class 0x02)...\n");
    int found_9260 = 0;
    int found_any_net = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            if (pci_vendor_id((uint8_t)bus, dev, 0) == 0xFFFF) continue;

            probe_net_function(bus, dev, 0, &found_9260, &found_any_net);

            uint8_t ht = pci_header_type((uint8_t)bus, dev, 0);
            if (ht & 0x80) { /* multi-function device */
                for (uint8_t func = 1; func < 8; func++) {
                    if (pci_vendor_id((uint8_t)bus, dev, func) != 0xFFFF) {
                        probe_net_function(bus, dev, func, &found_9260, &found_any_net);
                    }
                }
            }
        }
    }

    if (!found_any_net) {
        serial_writestring("[pci] no network-class PCI devices found\n");
    }
    if (!found_9260) {
        serial_writestring("[pci] Wireless-AC 9260 NOT found - check it's seated "
                            "and not disabled in BIOS/UEFI\n");
    }
    return found_9260;
}
