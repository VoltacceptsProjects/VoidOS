#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

struct multiboot_mmap_entry {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed));

#define MULTIBOOT_MEMORY_AVAILABLE 1

/* Bit 12 of multiboot_info.flags: framebuffer_* fields are valid. */
#define MULTIBOOT_INFO_FRAMEBUFFER_INFO (1 << 12)

/* multiboot_info.framebuffer_type values */
#define MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED 0
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB     1
#define MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT 2

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;

    /* Valid when flags bit 12 (MULTIBOOT_INFO_FRAMEBUFFER_INFO) is set.
     * Filled in by GRUB from the video mode fields in our Multiboot header;
     * this is what lets us draw text on UEFI machines, which have no
     * legacy 0xB8000 VGA text buffer. */
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;   /* bytes per scanline */
    uint32_t framebuffer_width;   /* pixels */
    uint32_t framebuffer_height;  /* pixels */
    uint8_t  framebuffer_bpp;     /* bits per pixel */
    uint8_t  framebuffer_type;    /* 0=indexed, 1=RGB, 2=EGA text */
    union {
        struct {
            uint32_t framebuffer_palette_addr;
            uint16_t framebuffer_palette_num_colors;
        } indexed;
        struct {
            uint8_t framebuffer_red_field_position;
            uint8_t framebuffer_red_mask_size;
            uint8_t framebuffer_green_field_position;
            uint8_t framebuffer_green_mask_size;
            uint8_t framebuffer_blue_field_position;
            uint8_t framebuffer_blue_mask_size;
        } rgb;
    };
} __attribute__((packed));

#endif
