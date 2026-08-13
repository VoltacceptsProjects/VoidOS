#include "pci.h"
#include "vga.h"
#include "io.h"
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
