/* Host-side sanity check for a packed .vapp file. Not part of the OS
 * build - reuses kernel/fs.h's real struct layout and reimplements the
 * same validation voidfs_install_vapp() performs (fs.c can't be
 * compiled standalone on the host: it calls into disk.c/vga.c, which
 * assume a freestanding x86 target), plus the same manifest parsing
 * apps.c uses, so packaging bugs get caught before ever booting the
 * ISO.
 *
 * Usage: gcc -Ikernel -o test_vapp tools/test_vapp.c && ./test_vapp vapps/notepad.vapp
 */
#include "../kernel/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has_vapp_suffix(const char* name, unsigned int cap) {
    unsigned int length = 0;
    while (length < cap && name[length]) length++;
    return length >= 5 && name[length - 5] == '.' && name[length - 4] == 'v' &&
           name[length - 3] == 'a' && name[length - 2] == 'p' && name[length - 1] == 'p';
}

static int text_equal(const char* a, const char* b, unsigned int cap) {
    unsigned int i = 0;
    while (i < cap && a[i] && b[i] && a[i] == b[i]) i++;
    return i < cap && a[i] == '\0' && b[i] == '\0';
}

/* Mirrors apps.c's parse_manifest() closely enough to prove the format
 * round-trips; kept intentionally simple like the on-target version. */
static void parse_manifest(const char* text, unsigned int length) {
    unsigned int i = 0;
    while (i < length) {
        unsigned int line_start = i;
        while (i < length && text[i] != '\n') i++;
        unsigned int line_end = i;
        if (i < length) i++;
        if (line_end > line_start && text[line_end - 1] == '\r') line_end--;
        unsigned int eq = line_start;
        while (eq < line_end && text[eq] != '=') eq++;
        if (eq >= line_end) continue;
        printf("  manifest: %.*s = %.*s\n", (int)(eq - line_start), text + line_start,
               (int)(line_end - eq - 1), text + eq + 1);
    }
}

int main(int argc, char** argv) {
    int failures = 0;
    for (int a = 1; a < argc; a++) {
        printf("== %s ==\n", argv[a]);
        FILE* f = fopen(argv[a], "rb");
        if (!f) { perror("fopen"); failures++; continue; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char* buf = malloc((size_t)size);
        if (fread(buf, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "short read\n"); failures++; }
        fclose(f);

        if ((size_t)size < sizeof(struct vapp_header)) { printf("FAIL: too small\n"); failures++; continue; }
        const struct vapp_header* h = (const struct vapp_header*)buf;

        int ok = 1;
        if (memcmp(h->magic, "VAPP", 4) != 0) { printf("FAIL: bad magic\n"); ok = 0; }
        if (h->format_version != 1) { printf("FAIL: format_version %u\n", h->format_version); ok = 0; }
        if (h->header_size < sizeof(struct vapp_header) || h->header_size > (uint32_t)size) { printf("FAIL: header_size\n"); ok = 0; }
        if (h->package_size != (uint32_t)size) { printf("FAIL: package_size %u != file size %ld\n", h->package_size, size); ok = 0; }
        if (!text_equal(h->mime, VOIDFS_VAPP_MIME, VOIDFS_MIME_MAX)) { printf("FAIL: mime mismatch: %.40s\n", h->mime); ok = 0; }
        if (h->manifest_offset > h->package_size || h->manifest_size > h->package_size - h->manifest_offset) { printf("FAIL: manifest bounds\n"); ok = 0; }
        if (h->payload_offset > h->package_size || h->payload_size > h->package_size - h->payload_offset) { printf("FAIL: payload bounds\n"); ok = 0; }
        if (h->name[0] && !has_vapp_suffix(h->name, VOIDFS_NAME_MAX)) { printf("FAIL: name must end .vapp: %.48s\n", h->name); ok = 0; }
        if (strlen(h->name) >= VOIDFS_NAME_MAX) { printf("FAIL: name too long\n"); ok = 0; }

        if (ok) {
            printf("OK  name=%.48s version=%.16s package_size=%u\n", h->name, h->app_version, h->package_size);
            parse_manifest((const char*)(buf + h->manifest_offset), h->manifest_size);
            printf("  payload (first 64 bytes): %.64s\n", buf + h->payload_offset);
        } else {
            failures++;
        }
        free(buf);
    }
    return failures ? 1 : 0;
}
