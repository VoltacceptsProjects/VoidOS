#ifndef NET_H
#define NET_H

#include <stdint.h>

struct multiboot_info;

/* Where this actually stands, stated as plainly as when this file said
 * "no network stack at all": the IP-and-above stack is now real and
 * complete for what an HTTP GET needs - see arp.c, ip.c, udp.c, tcp.c,
 * dhcp.c, dns.c. Each of those is a from-scratch implementation
 * against its RFC (826, 791, 768, 793, 2131, 1035 respectively) and is
 * internally consistent: correct checksums, correct sequence-number
 * arithmetic, correct state machines. What none of it has is a real
 * link layer under it. kernel/iwlwifi.c stops at "the firmware
 * booted" (see its own header) - there is still no 802.11 scan/auth/
 * association, no WPA2 4-way handshake, no CCMP encrypt/decrypt, and
 * no TX/RX queue servicing real hardware interrupts. netdev.h's
 * registered device is an honest null: it logs what it would have
 * sent and returns "accepted", but nothing goes on any wire and
 * nothing ever arrives, so netdev_rx() is never driven by a real
 * frame. That gap is the same one this file used to describe for the
 * whole stack - it just moved down to exactly one layer (802.11 MAC +
 * WPA2 + a working iwlwifi TX/RX datapath), which is still, on its
 * own, thousands of lines of register-level work that (per iwlwifi.h)
 * cannot be verified without real 9260 hardware in front of a serial
 * cable.
 *
 * net_http_get() is the seam every caller in this kernel that wants
 * "the network" (right now, just kernel/appstore.c) goes through. It
 * now genuinely attempts DHCP -> DNS -> TCP connect -> HTTP GET (see
 * net_init() and try_real_http_get() in net.c) every time it's called,
 * and will keep succeeding at that the moment a real link layer exists
 * under netdev - nothing in this file or any caller needs to change
 * then. Until it does, every real attempt times out for the reason
 * above, and this function falls back to checking whether the
 * requested path was staged onto the ISO as a Multiboot module, the
 * same mechanism .vapp and .ucode files already use (see
 * voidfs_install_multiboot_modules() in fs.c and
 * iwlwifi_find_firmware_module() in iwlwifi.c). That fallback is NOT a
 * network fetch and every caller is expected to treat it as a stand-in
 * test fixture, not as VoidOS reaching voidos.infinityfree.io. */

/* Brings up every protocol layer (netdev's null device, ARP, IP, UDP,
 * TCP) and attempts one DHCP exchange to learn a real IP/gateway/DNS
 * config. Safe to call even though nothing backs the null device with
 * a real transmitter yet (see netdev.h) - it will busy-poll for a
 * bounded number of iterations, log that nothing came back, and leave
 * the stack in its unconfigured state; net_http_get() below already
 * accounts for that and falls back to the Multiboot-module path in
 * that case. Call once from kernel_main(), after iwlwifi_stage3(). */
void net_init(void);

/* mbi is used only for the Multiboot-module fallback described above -
 * pass the same struct kernel_main() received.
 *
 * host/path describe the request the way a real HTTP client would (e.g.
 * host="voidos.infinityfree.io", path="/notepad.vapp"); host is
 * currently unused (there's nowhere to send it), kept as a parameter so
 * call sites already read correctly once this is real. path drives the
 * Multiboot-module fallback: matched against each module's filename
 * with path's leading '/' stripped.
 *
 * out/out_cap is caller-owned storage; on success *out_size is set to
 * the number of bytes copied into out (capped at out_cap - this never
 * overruns the buffer, and silently truncates if the real file would
 * have been bigger than out_cap).
 *
 * Returns 1 if bytes were obtained by whatever means and 0 otherwise -
 * check the serial log either way, it says which path was taken. */
int net_http_get(struct multiboot_info* mbi, const char* host, const char* path,
                  uint8_t* out, uint32_t out_cap, uint32_t* out_size);

#endif
