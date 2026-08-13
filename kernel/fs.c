#include "fs.h"
#include "multiboot.h"
#include "vga.h"

static uint8_t storage[VOIDFS_STORAGE_SIZE];
static struct voidfs_file directory[VOIDFS_MAX_FILES];
static unsigned int directory_count = 0;
static uint32_t storage_used = 0;

enum {
    VOIDFS_OK = 0,
    VOIDFS_BAD_PACKAGE = -1,
    VOIDFS_WRONG_MIME = -2,
    VOIDFS_NOT_VAPP = -3,
    VOIDFS_NO_SPACE = -4,
    VOIDFS_DIRECTORY_FULL = -5,
};

static unsigned int text_length(const char* text, unsigned int cap) {
    unsigned int n = 0;
    while (n < cap && text[n]) n++;
    return n;
}

static int text_equal(const char* a, const char* b, unsigned int cap) {
    unsigned int i = 0;
    while (i < cap && a[i] && b[i] && a[i] == b[i]) i++;
    return i < cap && a[i] == '\0' && b[i] == '\0';
}

static int has_vapp_suffix(const char* name, unsigned int cap) {
    unsigned int length = text_length(name, cap);
    return length >= 5 &&
           name[length - 5] == '.' &&
           name[length - 4] == 'v' &&
           name[length - 3] == 'a' &&
           name[length - 2] == 'p' &&
           name[length - 1] == 'p';
}

static const char* basename(const char* path) {
    const char* last = path;
    if (!path) return "";
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

static void copy_bounded(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    if (cap == 0) return;
    while (i + 1 < cap && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void voidfs_initialize(void) {
    directory_count = 0;
    storage_used = 0;
    for (unsigned int i = 0; i < VOIDFS_MAX_FILES; i++) {
        directory[i].name[0] = '\0';
        directory[i].mime[0] = '\0';
        directory[i].size = 0;
        directory[i].storage_offset = 0;
    }
}

int voidfs_install_vapp(const uint8_t* package, uint32_t package_size,
                        const char* source_name) {
    if (!package || package_size < sizeof(struct vapp_header)) {
        return VOIDFS_BAD_PACKAGE;
    }

    const struct vapp_header* header = (const struct vapp_header*)package;
    if (header->magic[0] != 'V' || header->magic[1] != 'A' ||
        header->magic[2] != 'P' || header->magic[3] != 'P') {
        return VOIDFS_NOT_VAPP;
    }
    if (header->format_version != 1 ||
        header->header_size < sizeof(struct vapp_header) ||
        header->header_size > package_size ||
        header->package_size != package_size) {
        return VOIDFS_BAD_PACKAGE;
    }
    if (!text_equal(header->mime, VOIDFS_VAPP_MIME, VOIDFS_MIME_MAX)) {
        return VOIDFS_WRONG_MIME;
    }
    if (header->manifest_offset > package_size ||
        header->manifest_size > package_size - header->manifest_offset ||
        header->payload_offset > package_size ||
        header->payload_size > package_size - header->payload_offset) {
        return VOIDFS_BAD_PACKAGE;
    }
    if (header->name[0] &&
        !has_vapp_suffix(header->name, VOIDFS_NAME_MAX)) {
        return VOIDFS_BAD_PACKAGE;
    }

    const char* package_name = header->name[0] ? header->name : basename(source_name);
    if (!has_vapp_suffix(package_name, header->name[0] ? VOIDFS_NAME_MAX : 256)) {
        return VOIDFS_BAD_PACKAGE;
    }
    if (text_length(package_name, VOIDFS_NAME_MAX) >= VOIDFS_NAME_MAX) {
        return VOIDFS_BAD_PACKAGE;
    }

    int existing = -1;
    for (unsigned int i = 0; i < directory_count; i++) {
        if (text_equal(directory[i].name, package_name, VOIDFS_NAME_MAX)) {
            existing = (int)i;
            break;
        }
    }

    uint32_t offset = storage_used;
    if (existing >= 0 && directory[existing].size >= package_size) {
        offset = directory[existing].storage_offset;
    } else if (package_size > VOIDFS_STORAGE_SIZE - storage_used) {
        return VOIDFS_NO_SPACE;
    }

    if (existing < 0) {
        if (directory_count >= VOIDFS_MAX_FILES) return VOIDFS_DIRECTORY_FULL;
        existing = (int)directory_count++;
    } else if (offset == storage_used) {
        storage_used += package_size;
    }

    for (uint32_t i = 0; i < package_size; i++) {
        storage[offset + i] = package[i];
    }
    if (offset == storage_used) storage_used += package_size;

    copy_bounded(directory[existing].name, package_name, VOIDFS_NAME_MAX);
    copy_bounded(directory[existing].mime, header->mime, VOIDFS_MIME_MAX);
    directory[existing].size = package_size;
    directory[existing].storage_offset = offset;
    return VOIDFS_OK;
}

void voidfs_install_multiboot_modules(struct multiboot_info* mbi) {
    if (!mbi || !mbi->mods_count || !mbi->mods_addr) return;
    struct multiboot_module* modules =
        (struct multiboot_module*)(uintptr_t)mbi->mods_addr;
    for (uint32_t i = 0; i < mbi->mods_count; i++) {
        const char* name = (const char*)(uintptr_t)modules[i].string;
        if (!name || !has_vapp_suffix(name, 256)) continue;
        const uint8_t* data = (const uint8_t*)(uintptr_t)modules[i].mod_start;
        uint32_t size = modules[i].mod_end - modules[i].mod_start;
        (void)voidfs_install_vapp(data, size, basename(name));
    }
}

unsigned int voidfs_file_count(void) {
    return directory_count;
}

const struct voidfs_file* voidfs_file_at(unsigned int index) {
    if (index >= directory_count) return (const struct voidfs_file*)0;
    return &directory[index];
}

int voidfs_read_file(unsigned int index, uint32_t offset,
                     uint8_t* out, uint32_t length) {
    if (index >= directory_count || !out) return -1;
    const struct voidfs_file* file = &directory[index];
    if (offset > file->size || length > file->size - offset) return -1;
    for (uint32_t i = 0; i < length; i++) {
        out[i] = storage[file->storage_offset + offset + i];
    }
    return 0;
}

void voidfs_print_files(void) {
    terminal_setcolor(VGA_WHITE, VGA_DARK_GREY);
    terminal_writestring("VoidFS\n\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    terminal_writestring("RAM-backed package volume\n");
    terminal_writestring("Boot modules ending in .vapp are installed automatically.\n\n");
    if (!directory_count) {
        terminal_setcolor(VGA_DARK_GREY, VGA_DARK_GREY);
        terminal_writestring("No applications installed.\n");
        terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
        return;
    }
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_DARK_GREY);
    terminal_writestring("Installed packages\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    for (unsigned int i = 0; i < directory_count; i++) {
        terminal_writestring("  ");
        terminal_writestring(directory[i].name);
        terminal_writestring("  ");
        terminal_writestring(directory[i].mime);
        terminal_writestring("  ");
        terminal_write_uint(directory[i].size);
        terminal_writestring(" bytes\n");
    }
}