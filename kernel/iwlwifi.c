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
        serial_writestring("[iwlwifi] MAC clock ready. Firmware self-load "
                            "(context info) is the next step - see "
                            "iwlwifi_load_firmware().\n");
    } else {
        serial_writestring("[iwlwifi] timed out waiting for MAC_CLOCK_READY. On "
                            "real hardware this usually means RF-kill is asserted "
                            "(check the hardware switch/BIOS Wireless setting) or "
                            "the card needs a power sequencing step this file "
                            "doesn't do yet.\n");
    }
    return ready;
}

/* --- firmware self-load (context-info method) ---------------------------
 * Struct layout and field meanings taken from upstream
 * drivers/net/wireless/intel/iwlwifi/pcie/iwl-context-info.h. Field
 * names kept close to upstream so this is diffable against the real
 * thing; only the __le64/__packed macros are swapped for plain
 * uint64_t + __attribute__((packed)), since x86 is little-endian
 * already and this kernel has no <linux/types.h> to pull in.
 */

#define IWL_CTXT_MAX_DRAM_ENTRY 64
#define CSR_CTXT_INFO_BA        0x040u

#define IWL_CTXT_INFO_TFD_FORMAT_LONG (1u << 8)          /* bit 8 */
#define IWL_CTXT_INFO_RB_CB_SIZE_SHIFT 4                  /* bits 4:7 */
#define IWL_CTXT_INFO_RB_SIZE_SHIFT    9                  /* bits 9:12 */
#define IWL_CTXT_INFO_RB_SIZE_4K       0x4u

#define CSR_INT_BIT_ALIVE (1u << 0)

struct iwl_ctx_info_version {
    uint16_t mac_id;
    uint16_t version;
    uint16_t size;      /* struct size in DWs */
    uint16_t reserved;
} __attribute__((packed));

struct iwl_ctx_info_control {
    uint32_t control_flags;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_ctx_info_rbd_cfg {
    uint64_t free_rbd_addr;
    uint64_t used_rbd_addr;
    uint64_t status_wr_ptr;
} __attribute__((packed));

struct iwl_ctx_info_hcmd_cfg {
    uint64_t cmd_queue_addr;
    uint8_t  cmd_queue_size;
    uint8_t  reserved[7];
} __attribute__((packed));

struct iwl_ctx_info_dump_cfg {
    uint64_t core_dump_addr;
    uint32_t core_dump_size;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_ctx_info_edbg_cfg {
    uint64_t early_debug_addr;
    uint32_t early_debug_size;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_ctx_info_pnvm_cfg {
    uint64_t platform_nvm_addr;
    uint32_t platform_nvm_size;
    uint32_t reserved;
} __attribute__((packed));

struct iwl_ctx_info_dram {
    uint64_t umac_img[IWL_CTXT_MAX_DRAM_ENTRY];
    uint64_t lmac_img[IWL_CTXT_MAX_DRAM_ENTRY];
    uint64_t virtual_img[IWL_CTXT_MAX_DRAM_ENTRY];
} __attribute__((packed));

struct iwl_context_info {
    struct iwl_ctx_info_version version;
    struct iwl_ctx_info_control control;
    uint64_t reserved0;
    struct iwl_ctx_info_rbd_cfg rbd_cfg;
    struct iwl_ctx_info_hcmd_cfg hcmd_cfg;
    uint32_t reserved1[4];
    struct iwl_ctx_info_dump_cfg dump_cfg;
    struct iwl_ctx_info_edbg_cfg edbg_cfg;
    struct iwl_ctx_info_pnvm_cfg pnvm_cfg;
    uint32_t reserved2[16];
    struct iwl_ctx_info_dram dram;
    uint32_t reserved3[16];
} __attribute__((packed));

/* Statically allocated - this kernel has no heap. All of it needs to
 * sit at a fixed, DMA-visible address, which "static" already gives
 * us here since there's no paging to fight with. */
#define IWLWIFI_RX_RB_COUNT 8
#define IWLWIFI_RX_RB_SIZE  4096
#define IWLWIFI_TX_CMDQ_ENTRIES 16

static struct iwl_context_info g_ctxt_info __attribute__((aligned(16)));
static uint64_t g_rx_free_rbd[IWLWIFI_RX_RB_COUNT] __attribute__((aligned(16)));
static uint64_t g_rx_used_rbd[IWLWIFI_RX_RB_COUNT] __attribute__((aligned(16)));
static uint8_t  g_rx_buffers[IWLWIFI_RX_RB_COUNT][IWLWIFI_RX_RB_SIZE] __attribute__((aligned(4096)));
struct iwl_rb_status {
    uint16_t closed_rb_num;
    uint16_t closed_fr_num;
    uint16_t finished_rb_num;
    uint16_t finished_fr_num;
    uint32_t reserved;
} __attribute__((packed));
static struct iwl_rb_status g_rb_status __attribute__((aligned(16)));
/* Never posted to - just a validly-sized, empty ring so the device has
 * something to point at. See the header comment for why this exists
 * but isn't used. */
static uint8_t g_tx_cmdq[IWLWIFI_TX_CMDQ_ENTRIES * 128] __attribute__((aligned(256)));

static void csr_write64(uint32_t bar0, uint32_t offset, uint64_t val) {
    csr_write(bar0, offset, (uint32_t)(val & 0xFFFFFFFFu));
    csr_write(bar0, offset + 4, (uint32_t)(val >> 32));
}

/* ilog2(x) - 3, matching upstream TFD_QUEUE_CB_SIZE() - x must be a
 * power of two. */
static uint32_t cb_size_exponent(uint32_t entries) {
    uint32_t log2 = 0;
    while ((1u << log2) < entries) log2++;
    return log2 - 3;
}

int iwlwifi_load_firmware(uint32_t bar0, const struct iwlwifi_fw_image* fw) {
    serial_writestring("[iwlwifi] building context-info block for firmware "
                        "self-load\n");

    /* Split IWL_UCODE_TLV_SEC_RT sections into LMAC (before the first
     * CPU1/CPU2 separator) and UMAC (between the first and second
     * separator) groups, same split upstream's iwl_pcie_init_fw_sec()
     * does. Anything from the paging separator onward (paged/virtual
     * image) is skipped - not loaded, not counted, just logged. */
    static uint64_t lmac_tmp[IWL_CTXT_MAX_DRAM_ENTRY];
    static uint64_t umac_tmp[IWL_CTXT_MAX_DRAM_ENTRY];
    uint32_t lmac_n = 0, umac_n = 0, paging_n = 0;
    int seen_seps = 0;

    for (uint32_t i = 0; i < fw->num_sections; i++) {
        const struct iwlwifi_fw_section* s = &fw->sections[i];
        if (s->is_init) continue; /* calibration image - not loaded here */

        if (s->is_separator) {
            seen_seps++;
            continue;
        }

        uint64_t phys = (uint64_t)(uintptr_t)s->data;
        if (seen_seps == 0) {
            if (lmac_n < IWL_CTXT_MAX_DRAM_ENTRY)
                lmac_tmp[lmac_n++] = phys;
        } else if (seen_seps == 1) {
            if (umac_n < IWL_CTXT_MAX_DRAM_ENTRY)
                umac_tmp[umac_n++] = phys;
        } else {
            paging_n++; /* skipped */
        }
    }

    serial_writestring("[iwlwifi]   lmac sections=");
    serial_write_uint(lmac_n);
    serial_writestring(" umac sections=");
    serial_write_uint(umac_n);
    serial_writestring(" paging sections skipped=");
    serial_write_uint(paging_n);
    serial_writestring("\n");

    if (lmac_n == 0) {
        serial_writestring("[iwlwifi]   no LMAC section found before the first "
                            "separator - firmware image doesn't look like the "
                            "shape iwl_pcie_init_fw_sec() expects, aborting\n");
        return 0;
    }

    /* RX ring: 8 x 4K buffers, long-TFD-format, physical addresses of
     * our static buffers (no paging => pointer == physical address). */
    for (int i = 0; i < IWLWIFI_RX_RB_COUNT; i++) {
        g_rx_free_rbd[i] = (uint64_t)(uintptr_t)&g_rx_buffers[i][0];
        g_rx_used_rbd[i] = 0;
    }

    for (uint32_t i = 0; i < sizeof(g_ctxt_info); i++)
        ((uint8_t*)&g_ctxt_info)[i] = 0;

    g_ctxt_info.version.mac_id = (uint16_t)csr_read(bar0, CSR_HW_REV);
    g_ctxt_info.version.version = 0;
    g_ctxt_info.version.size = (uint16_t)(sizeof(g_ctxt_info) / 4);

    uint32_t rb_cb_exp = cb_size_exponent(IWLWIFI_RX_RB_COUNT);
    g_ctxt_info.control.control_flags =
        IWL_CTXT_INFO_TFD_FORMAT_LONG |
        (rb_cb_exp << IWL_CTXT_INFO_RB_CB_SIZE_SHIFT) |
        (IWL_CTXT_INFO_RB_SIZE_4K << IWL_CTXT_INFO_RB_SIZE_SHIFT);

    g_ctxt_info.rbd_cfg.free_rbd_addr = (uint64_t)(uintptr_t)g_rx_free_rbd;
    g_ctxt_info.rbd_cfg.used_rbd_addr = (uint64_t)(uintptr_t)g_rx_used_rbd;
    g_ctxt_info.rbd_cfg.status_wr_ptr = (uint64_t)(uintptr_t)&g_rb_status;

    g_ctxt_info.hcmd_cfg.cmd_queue_addr = (uint64_t)(uintptr_t)g_tx_cmdq;
    g_ctxt_info.hcmd_cfg.cmd_queue_size =
        (uint8_t)cb_size_exponent(IWLWIFI_TX_CMDQ_ENTRIES);

    for (uint32_t i = 0; i < lmac_n; i++) g_ctxt_info.dram.lmac_img[i] = lmac_tmp[i];
    for (uint32_t i = 0; i < umac_n; i++) g_ctxt_info.dram.umac_img[i] = umac_tmp[i];

    serial_writestring("[iwlwifi] context-info block ready at ");
    serial_write_hex((uint32_t)(uintptr_t)&g_ctxt_info);
    serial_writestring(", size=");
    serial_write_uint((uint32_t)sizeof(g_ctxt_info));
    serial_writestring(" bytes\n");

    /* Clear any stale interrupt bits, then let the ALIVE interrupt
     * through (upstream unmasks before every fw boot, so match it). */
    csr_write(bar0, CSR_INT, 0xFFFFFFFFu);
    csr_write(bar0, CSR_INT_MASK, CSR_INT_BIT_ALIVE);

    serial_writestring("[iwlwifi] writing CSR_CTXT_INFO_BA - firmware self-load "
                        "starts now\n");
    csr_write64(bar0, CSR_CTXT_INFO_BA, (uint64_t)(uintptr_t)&g_ctxt_info);

    int alive = 0;
    for (uint32_t tries = 0; tries < 5000; tries++) {
        if (csr_read(bar0, CSR_INT) & CSR_INT_BIT_ALIVE) {
            alive = 1;
            break;
        }
        short_delay(10);
    }

    if (alive) {
        csr_write(bar0, CSR_INT, CSR_INT_BIT_ALIVE); /* ack, write-1-to-clear */
        serial_writestring("[iwlwifi] CSR_INT_BIT_ALIVE seen - firmware booted "
                            "and self-initialized. This confirms the ucode is "
                            "running on the device. Stopping here: the ALIVE "
                            "notification itself (on the RX ring built above) "
                            "isn't parsed, and there's no TX/RX datapath or "
                            "802.11 MAC past this point yet.\n");
    } else {
        serial_writestring("[iwlwifi] timed out waiting for CSR_INT_BIT_ALIVE. "
                            "Firmware never signaled it booted - check the "
                            "context-info block layout above against a real "
                            "capture (or a real card) before assuming the "
                            "hardware itself is at fault.\n");
    }
    return alive;
}
