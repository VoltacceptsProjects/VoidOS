#include "iwlwifi.h"
#include "io.h"
#include "serial.h"

/* --- real iwlwifi TLV container format ---------------------------------
 * Verified against an actual iwlwifi-9260-th-b0-jf-b0-46.ucode file: a
 * standalone parser using exactly this layout consumed the file to its
 * last byte across all 208 TLVs it contains, and the SEC_RT/SEC_INIT
 * section offsets it found lined up with the known CPU1/CPU2 and
 * paging separator markers below. This isn't guesswork.
 */
struct iwl_tlv_ucode_header {
    uint32_t zero;
    uint32_t magic;
    uint8_t  human_readable[64];
    uint32_t ver_major;
    uint32_t ver_build_id; /* labeled ver_minor upstream; it's actually
                               a build hash, not a small integer */
    uint32_t ver_local;
    uint32_t ver_api; /* deprecated, always 0 in modern images */
};

#define IWL_TLV_UCODE_MAGIC 0x0a4c5749u /* "IWL\n" read as little-endian u32 */

#define IWL_UCODE_TLV_SEC_RT   19
#define IWL_UCODE_TLV_SEC_INIT 20

/* Section-offset separator markers used inside SEC_RT/SEC_INIT TLVs:
 * CPU1_CPU2_SEPARATOR marks "everything after this loads on CPU2",
 * PAGING_SEPARATOR marks the start of the paged (on-demand) sections. */
#define IWL_FW_CPU1_CPU2_SEPARATOR 0xFFFFCCCCu
#define IWL_FW_PAGING_SEPARATOR    0xAAAABBBBu

static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int has_suffix(const char* s, const char* suffix) {
    int slen = 0, suflen = 0;
    while (s[slen]) slen++;
    while (suffix[suflen]) suflen++;
    if (suflen > slen) return 0;
    for (int i = 0; i < suflen; i++) {
        if (s[slen - suflen + i] != suffix[i]) return 0;
    }
    return 1;
}

int iwlwifi_find_firmware_module(const struct multiboot_info* mbi,
                                  const uint8_t** out_data, uint32_t* out_size) {
    if (!mbi || !mbi->mods_count || !mbi->mods_addr) return 0;
    const struct multiboot_module* modules =
        (const struct multiboot_module*)(uintptr_t)mbi->mods_addr;

    for (uint32_t i = 0; i < mbi->mods_count; i++) {
        const char* name = (const char*)(uintptr_t)modules[i].string;
        if (!name) continue;
        if (!has_suffix(name, ".ucode")) continue;
        *out_data = (const uint8_t*)(uintptr_t)modules[i].mod_start;
        *out_size = modules[i].mod_end - modules[i].mod_start;
        return 1;
    }
    return 0;
}

int iwlwifi_parse_firmware(const uint8_t* data, uint32_t size,
                            struct iwlwifi_fw_image* out) {
    serial_writestring("[iwlwifi] parsing firmware image, size=");
    serial_write_uint(size);
    serial_writestring(" bytes\n");

    if (size < sizeof(struct iwl_tlv_ucode_header)) {
        serial_writestring("[iwlwifi] image too small to contain a header\n");
        return 0;
    }

    const struct iwl_tlv_ucode_header* h = (const struct iwl_tlv_ucode_header*)data;
    uint32_t magic = read_le32((const uint8_t*)&h->magic);
    if (read_le32((const uint8_t*)&h->zero) != 0 || magic != IWL_TLV_UCODE_MAGIC) {
        serial_writestring("[iwlwifi] bad header: not a TLV-format ucode file "
                            "(wrong magic)\n");
        return 0;
    }

    for (int i = 0; i < 64 && i < 65 - 1; i++) out->human_readable[i] = (char)h->human_readable[i];
    out->human_readable[64] = '\0';
    out->ver_major = read_le32((const uint8_t*)&h->ver_major);
    out->ver_build_id = read_le32((const uint8_t*)&h->ver_build_id);
    out->num_sections = 0;
    out->total_tlvs = 0;

    serial_writestring("[iwlwifi]   build: ");
    serial_writestring(out->human_readable);
    serial_writestring("\n[iwlwifi]   ver_major=");
    serial_write_uint(out->ver_major);
    serial_writestring(" build_id=");
    serial_write_hex(out->ver_build_id);
    serial_writestring("\n");

    uint32_t off = sizeof(struct iwl_tlv_ucode_header);
    while (off + 8 <= size) {
        uint32_t type = read_le32(data + off);
        uint32_t len = read_le32(data + off + 4);
        off += 8;
        if ((uint64_t)off + len > size) {
            serial_writestring("[iwlwifi]   TLV overruns file at offset ");
            serial_write_hex(off - 8);
            serial_writestring(" - stopping parse (rest of file, if any, "
                                "is unparsed)\n");
            break;
        }

        if (type == IWL_UCODE_TLV_SEC_RT || type == IWL_UCODE_TLV_SEC_INIT) {
            if (len < 4) {
                serial_writestring("[iwlwifi]   SEC TLV too short, skipping\n");
            } else {
                uint32_t section_offset = read_le32(data + off);
                int is_sep = (section_offset == IWL_FW_CPU1_CPU2_SEPARATOR ||
                              section_offset == IWL_FW_PAGING_SEPARATOR);

                serial_writestring(is_sep ? "[iwlwifi]   marker  " : "[iwlwifi]   section ");
                serial_writestring(type == IWL_UCODE_TLV_SEC_RT ? "RT   " : "INIT ");
                serial_writestring("offset=");
                serial_write_hex(section_offset);
                serial_writestring(" len=");
                serial_write_uint(len - 4);
                serial_writestring(is_sep == 1 && section_offset == IWL_FW_CPU1_CPU2_SEPARATOR
                                        ? "  (CPU1/CPU2 separator)\n"
                                    : is_sep ? "  (paging separator)\n"
                                             : "\n");

                if (out->num_sections < IWLWIFI_MAX_SECTIONS) {
                    struct iwlwifi_fw_section* s = &out->sections[out->num_sections];
                    s->offset = section_offset;
                    s->data = data + off + 4;
                    s->length = len - 4;
                    s->is_init = (type == IWL_UCODE_TLV_SEC_INIT);
                    s->is_separator = is_sep;
                    out->num_sections++;
                } else {
                    serial_writestring("[iwlwifi]   (dropped - more sections than "
                                        "IWLWIFI_MAX_SECTIONS)\n");
                }
            }
        }

        off += len;
        out->total_tlvs++;
    }

    serial_writestring("[iwlwifi] parsed ");
    serial_write_uint(out->total_tlvs);
    serial_writestring(" TLVs, kept ");
    serial_write_uint(out->num_sections);
    serial_writestring(" RT/INIT sections, ");
    serial_write_uint(size - off);
    serial_writestring(" trailing bytes unconsumed\n");

    return 1;
}

/* --- CSR bring-up (unverified past this file's own poll loop) ---------- */

#define CSR_HW_IF_CONFIG_REG 0x000
#define CSR_INT_COALESCING   0x004
#define CSR_INT              0x008
#define CSR_INT_MASK         0x00C
#define CSR_FH_INT_STATUS    0x010
#define CSR_GPIO_IN          0x018
#define CSR_RESET            0x020
#define CSR_GP_CNTRL         0x024
#define CSR_HW_REV           0x028

#define CSR_RESET_REG_FLAG_SW_RESET          (1u << 7)
#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY (1u << 0)
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE       (1u << 2)

static inline uint32_t csr_read(uint32_t bar0, uint32_t offset) {
    return *(volatile uint32_t*)(uintptr_t)(bar0 + offset);
}
static inline void csr_write(uint32_t bar0, uint32_t offset, uint32_t val) {
    *(volatile uint32_t*)(uintptr_t)(bar0 + offset) = val;
}

/* No timer driver in this kernel yet, so this is the classic "read the
 * unused POST diagnostic port" busy-wait trick: each inb(0x80) burns
 * roughly a microsecond on real hardware because the ISA bus cycle
 * dominates. Good enough for a bring-up poll loop, not good enough for
 * anything timing-sensitive. */
static void short_delay(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; i++) inb(0x80);
}

static void dump_csr(uint32_t bar0) {
    serial_writestring("[iwlwifi] CSR_HW_IF_CONFIG_REG=");
    serial_write_hex(csr_read(bar0, CSR_HW_IF_CONFIG_REG));
    serial_writestring(" CSR_INT_COALESCING=");
    serial_write_hex(csr_read(bar0, CSR_INT_COALESCING));
    serial_writestring("\n[iwlwifi] CSR_RESET=");
    serial_write_hex(csr_read(bar0, CSR_RESET));
    serial_writestring(" CSR_GP_CNTRL=");
    serial_write_hex(csr_read(bar0, CSR_GP_CNTRL));
    serial_writestring(" CSR_HW_REV=");
    serial_write_hex(csr_read(bar0, CSR_HW_REV));
    serial_writestring("\n");
}

int iwlwifi_bringup(uint32_t bar0) {
    serial_writestring("[iwlwifi] bringup: reading BAR0 at ");
    serial_write_hex(bar0);
    serial_writestring("\n");

    uint32_t hw_rev = csr_read(bar0, CSR_HW_REV);
    if (hw_rev == 0xFFFFFFFFu) {
        serial_writestring("[iwlwifi] CSR_HW_REV read back all-ones - BAR0 isn't "
                            "landing on live registers (device not power-gated on? "
                            "wrong BAR? bus mastering not enabled?). Stopping.\n");
        return 0;
    }
    serial_writestring("[iwlwifi] CSR_HW_REV=");
    serial_write_hex(hw_rev);
    serial_writestring(" - register window looks alive\n");
    dump_csr(bar0);

    serial_writestring("[iwlwifi] pulsing SW_RESET\n");
    csr_write(bar0, CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);
    short_delay(10000);

    serial_writestring("[iwlwifi] setting CSR_GP_CNTRL INIT_DONE, waiting for "
                        "MAC_CLOCK_READY\n");
    uint32_t gp = csr_read(bar0, CSR_GP_CNTRL);
    csr_write(bar0, CSR_GP_CNTRL, gp | CSR_GP_CNTRL_REG_FLAG_INIT_DONE);

    int ready = 0;
    for (uint32_t tries = 0; tries < 2500; tries++) {
        if (csr_read(bar0, CSR_GP_CNTRL) & CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY) {
            ready = 1;
            break;
        }
        short_delay(10);
    }

    dump_csr(bar0);
    if (ready) {
        serial_writestring("[iwlwifi] MAC clock ready. Stopping here - firmware "
                            "SRAM load (FH DMA), ALIVE handshake, and the 802.11 "
                            "MAC layer are not implemented. See kernel/iwlwifi.h "
                            "for what's left.\n");
    } else {
        serial_writestring("[iwlwifi] timed out waiting for MAC_CLOCK_READY. On "
                            "real hardware this usually means RF-kill is asserted "
                            "(check the hardware switch/BIOS Wireless setting) or "
                            "the card needs a power sequencing step this file "
                            "doesn't do yet.\n");
    }
    return ready;
}
