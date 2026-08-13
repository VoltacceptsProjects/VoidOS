#ifndef IWLWIFI_H
#define IWLWIFI_H

#include <stdint.h>
#include "multiboot.h"

/* Stage 3: Intel Wireless-AC 9260 firmware load / NIC bring-up.
 *
 * Scope of what this file actually does, honestly stated:
 *
 *   1. Finds the .ucode firmware image among the Multiboot modules
 *      GRUB handed us (same mechanism the .vapp packages use - see
 *      voidfs_install_multiboot_modules() in fs.c).
 *   2. Parses the real iwlwifi TLV firmware container format and logs
 *      every section it finds over serial, so you can confirm the
 *      image you baked into the ISO is intact and being read
 *      correctly. This part is fully exercised by the code as
 *      written - the TLV walker below was validated byte-for-byte
 *      against a real iwlwifi-9260 firmware file before being ported
 *      in here (it consumes the file to the exact last byte and finds
 *      the known CPU1/CPU2 and paging separator markers).
 *   3. Does the first step of real hardware bring-up: pulses
 *      CSR_RESET and waits for CSR_GP_CNTRL to report the MAC clock
 *      is running. That's as far as this goes.
 *
 * What this file does NOT do: transfer the parsed firmware sections
 * into the device's SRAM over the FH DMA engine, wait for the
 * ALIVE notification, bring up TX/RX queues, or anything resembling
 * an 802.11 MAC (scanning, auth, association, encryption, a data
 * path). That is the bulk of what a real iwlwifi driver is, it's
 * thousands of lines even in Linux, and none of it can be verified
 * without real 9260 hardware in front of a serial cable - so rather
 * than hand you plausible-looking register pokes for a stage no one
 * has tested, this stops at a verifiable checkpoint and logs clearly
 * where it stopped. The OSDev wiki's "Iwlwifi" and "Intel Dual Band
 * Wireless" pages are the best next reference for continuing past
 * this point; even that community writeup is explicitly WIP for the
 * firmware-load and NIC-init steps, which tells you something about
 * how much hardware-in-hand iteration those steps need.
 */

#define IWLWIFI_MAX_SECTIONS 32

struct iwlwifi_fw_section {
    uint32_t offset;        /* load-address hint from the TLV, or a
                                separator marker (see is_separator) */
    const uint8_t* data;    /* pointer into the firmware image */
    uint32_t length;        /* payload length, not counting the
                                4-byte offset prefix */
    int is_init;            /* 1 = SEC_INIT (type 20), 0 = SEC_RT (type 19) */
    int is_separator;       /* 1 if offset is a marker (0xFFFFCCCC /
                                0xAAAABBBB) rather than a real address */
};

struct iwlwifi_fw_image {
    char human_readable[65];
    uint32_t ver_major;
    uint32_t ver_build_id;
    struct iwlwifi_fw_section sections[IWLWIFI_MAX_SECTIONS];
    uint32_t num_sections;
    uint32_t total_tlvs;
};

/* Scans Multiboot modules for a file whose name ends in ".ucode".
 * Returns 1 and fills out_data/out_size if found, 0 otherwise. */
int iwlwifi_find_firmware_module(const struct multiboot_info* mbi,
                                  const uint8_t** out_data, uint32_t* out_size);

/* Parses the TLV container format and logs a summary + every
 * SEC_RT/SEC_INIT section over serial. Returns 1 on a structurally
 * valid image, 0 if the magic/header didn't match. */
int iwlwifi_parse_firmware(const uint8_t* data, uint32_t size,
                            struct iwlwifi_fw_image* out);

/* Best-effort, unverified-on-hardware NIC wake-up: pulses CSR_RESET,
 * sets the INIT_DONE bit in CSR_GP_CNTRL, and polls for
 * MAC_CLOCK_READY. Logs every step and the final CSR register dump
 * over serial regardless of outcome. Returns 1 if the clock came up
 * within the timeout, 0 otherwise. bar0 must be the physical address
 * of the 9260's BAR0 MMIO window (see pci_probe_network_devices()). */
int iwlwifi_bringup(uint32_t bar0);

/* --- firmware self-load (context-info method) --------------------------
 *
 * Correction to the note above: the 9260 is an "8000 family" part
 * (Thunder Peak MAC / Jefferson Peak RF). Those parts, unlike the
 * older 3160/7260-era chips, do NOT use the FH_TCSR/TFDIB per-chunk
 * DMA channel to push firmware into SRAM. They use a newer, simpler
 * "context info" self-load: the driver builds one struct in memory
 * describing where each firmware section lives (by physical address),
 * points a single CSR register at it, and the device DMAs everything
 * in itself. Confirmed by reading the current upstream iwlwifi source
 * (drivers/net/wireless/intel/iwlwifi/pcie/ctxt-info.c and
 * iwl-context-info.h) - the struct layout and CSR_CTXT_INFO_BA offset
 * below are taken from there, not guessed.
 *
 * What iwlwifi_load_firmware() actually does:
 *   1. Splits the already-parsed IWL_UCODE_TLV_SEC_RT sections into
 *      LMAC / UMAC groups using the CPU1/CPU2 separator marker (same
 *      split iwl_pcie_init_fw_sec() does upstream). The IWL_UCODE_TLV_SEC_INIT
 *      (calibration) image and anything after the paging separator
 *      are not loaded - see the honest gaps below.
 *   2. Points the context-info DRAM map entries directly at the
 *      firmware section bytes already sitting in the Multiboot module
 *      - this kernel never enables paging, so a pointer here already
 *      *is* the physical address the device needs, with no bounce
 *      buffer or copy required (that's a genuine simplification a
 *      paging OS like Linux doesn't get).
 *   3. Builds a minimal 8-entry RX buffer ring and an empty (never
 *      posted-to) TX command ring, because the context-info struct
 *      has no "optional" field for either - the device expects both
 *      to be present and validly sized before it will boot, even
 *      though nothing past this file's own poll loop uses them.
 *   4. Writes the struct's physical address to CSR_CTXT_INFO_BA (CSR
 *      offset 0x40, a 64-bit register) to kick off self-load, then
 *      polls CSR_INT for CSR_INT_BIT_ALIVE (bit 0) - "uCode interrupts
 *      once it initializes", straight from the upstream comment on
 *      that bit.
 *
 * What this does NOT do, stated as plainly as the file above does:
 *   - It does not parse the ALIVE notification the firmware posts to
 *     the RX ring (UMAC/LMAC error-table pointers, firmware status,
 *     etc). The 8-entry RX ring exists only so the device has
 *     somewhere valid to land that packet; this file never reads it.
 *     Getting from "the ALIVE interrupt fired" to "I decoded the
 *     ALIVE command" is most of what a real RX transport is.
 *   - It does not load the INIT/calibration image, service any host
 *     command, bring up TX, or implement any 802.11 MAC behavior.
 *     Same scope boundary as iwlwifi_bringup() above, just moved one
 *     checkpoint further: this now gets far enough to tell you,
 *     definitively and from real hardware, whether the firmware
 *     itself booted - which CSR_GP_CNTRL alone never could.
 *
 * Call only after iwlwifi_bringup() has returned 1 (MAC clock must be
 * running before the device will act on CSR_CTXT_INFO_BA). Returns 1
 * if CSR_INT_BIT_ALIVE was observed within the timeout, 0 otherwise -
 * check the serial log either way, it explains what it saw. */
int iwlwifi_load_firmware(uint32_t bar0, const struct iwlwifi_fw_image* fw);

/* --- RX transport: draining the ring built above, and host commands ----
 *
 * Scope of what's added here, same honesty rule as everything above:
 *
 *   iwlwifi_service_alive() reads the RX ring iwlwifi_load_firmware()
 *   already built and decodes whatever landed in it using the real
 *   iwl_rx_packet / iwl_cmd_header wire format - verified against
 *   drivers/net/wireless/intel/iwlwifi/iwl-trans.h and
 *   fw/api/cmdhdr.h, not guessed (len_n_flags + FH_RSCSR_FRAME_SIZE_MSK,
 *   the cmd/group_id/sequence header, all confirmed byte-for-byte).
 *   It confirms the packet is genuinely UCODE_ALIVE_NTFY (cmd 0x1,
 *   group 0) and fully decodes struct iwl_alive_ntf_v3 - status, flags,
 *   iwl_lmac_alive (ucode_major/minor, ver_subtype, ver_type, mac, opt,
 *   timestamp, iwl_lmac_debug_addrs dbg_ptrs), and iwl_umac_alive
 *   (umac_major/minor, iwl_umac_debug_addrs dbg_ptrs), all confirmed
 *   byte-for-byte against the real drivers/net/wireless/intel/iwlwifi/
 *   fw/api/alive.h upstream source (plus the stable-tree fix for the
 *   ucode_major/minor ordering). This closed a real bug: pkt->data is
 *   the whole iwl_alive_ntf_v3 payload, and status+flags (4 bytes) come
 *   before lmac_data - the previous version of this parser started
 *   reading ucode_major at offset 0 and was silently off by 4 bytes on
 *   every field. v3 is the right shape for this 9260-class device; if a
 *   real capture ever shows payload_len not matching sizeof(that struct)
 *   exactly, that's the signal the firmware handed back v7/v8 instead
 *   and this needs to grow to match - iwlwifi_service_alive() already
 *   hex-dumps that mismatch case instead of misreading it.
 *
 *   iwlwifi_send_host_cmd() / iwlwifi_echo_test() build a real TFD -
 *   the "gen1" 128-byte format (3 reserved bytes, num_tbs, 20
 *   (lo32,hi_len16) fragment descriptors, 4-byte pad) that's exactly
 *   why g_tx_cmdq was already sized at IWLWIFI_TX_CMDQ_ENTRIES * 128
 *   above - into the command queue, point its one fragment at a real
 *   iwl_cmd_header + payload sitting in a static buffer, and ring
 *   HBUS_TARG_WRPTR (HBUS_BASE+0x060 - the +0x060 and the "bits 11:8
 *   select the queue" layout are confirmed from a real iwl-csr.h diff
 *   this session; HBUS_BASE itself is carried from memory as 0x400,
 *   the same as every iwlwifi generation back to the 3945, but that
 *   specific constant was not re-fetched from source this session, so
 *   treat CSR 0x460 as "very likely right" rather than "verified" like
 *   the rest of this file's register offsets) to tell the device a new
 *   TFD is posted. iwlwifi_echo_test() exercises this with
 *   ECHO_CMD (0x3, group 0), documented upstream as existing exactly
 *   for this: send data, get it back unchanged.
 *
 *   What's genuinely unverified here, stated plainly: which queue
 *   index the firmware expects this minimal, pre-scheduler-config
 *   command queue to answer on. Normal MVM operation uses a queue the
 *   driver explicitly configures via SCD_QUEUE_CFG first; nothing in
 *   this file has done that, because context-info self-load is
 *   supposed to hand the device one working command queue before any
 *   of that setup exists. Queue 0 is used below as the only queue that
 *   plausibly exists this early - if ALIVE fires but
 *   iwlwifi_echo_test() times out, a wrong queue index here is the
 *   first thing to check against a real capture, before assuming the
 *   TFD or header framing is wrong (those are the parts verified
 *   against upstream source above).
 *
 * Call iwlwifi_service_alive() only after iwlwifi_load_firmware() has
 * returned 1. Returns 1 if a decodable ALIVE packet was found, 0
 * otherwise - check the serial log either way.
 *
 * Call iwlwifi_echo_test() only after iwlwifi_service_alive() has
 * returned 1 (no point probing the command path before we've confirmed
 * the RX path can deliver a response). Returns 1 if the echoed payload
 * came back intact, 0 on timeout or mismatch. */
int iwlwifi_service_alive(uint32_t bar0);

/* Builds an iwl_cmd_header + payload, posts it as a one-fragment TFD on
 * the command queue, and rings HBUS_TARG_WRPTR. Does not wait for a
 * response - callers that need one poll find_rx_packet()'s job
 * themselves (see iwlwifi_echo_test() for the pattern) since what
 * counts as "the response" differs per command. Returns 1 if the
 * command was posted (says nothing about whether the device accepted
 * or answered it), 0 if payload_len didn't fit in the command buffer. */
int iwlwifi_send_host_cmd(uint32_t bar0, uint8_t cmd_id,
                           const void* payload, uint16_t payload_len);

int iwlwifi_echo_test(uint32_t bar0);

#endif
