#ifndef NETDEV_H
#define NETDEV_H

#include <stdint.h>

/* This is the seam between "protocol logic" (ARP/IP/TCP, all in this
 * directory) and "however frames actually get on the wire" (802.11 +
 * WPA2 + the iwlwifi TX/RX queues - see iwlwifi.h for exactly how far
 * that gets today, which is: not to a working data path).
 *
 * Everything above this line (arp.c, ip.c, tcp.c) only ever calls
 * netdev_tx() and only ever gets called back through netdev_rx() - it
 * has no idea whether the frames underneath are real 802.11 data
 * frames decrypted off real hardware, or nothing at all. That split is
 * deliberate: it means the protocol layers can be written and be
 * *correct* (checksums, state machines, byte layouts - all things you
 * can verify without hardware) independently of whether a real
 * transmitter exists yet, and nothing above this line has to change
 * when one does.
 *
 * As of this file, exactly one netdev is registered by net.c: a null
 * device (see netdev_null_init() in netdev.c) whose tx function logs
 * the frame over serial and returns "sent" without putting a single
 * bit on any wire, and whose rx path is never driven because nothing
 * feeds it real 802.11 frames. That is NOT a network connection. It
 * exists so the protocol layers below have something to link against
 * and so their logic can be exercised (loopback-style, or against a
 * test harness) before real hardware TX/RX exists. Wiring a real
 * device in means writing netdev_register() with a struct netdev_ops
 * backed by the iwlwifi data path once that exists - see iwlwifi.h for
 * the (substantial) list of what has to happen first: ALIVE handling,
 * a real TX queue, 802.11 auth/association, the WPA2 4-way handshake,
 * and CCMP encrypt/decrypt on every frame. */

#define NETDEV_MTU 1500

struct netdev_ops {
    /* Send one Ethernet-II frame (dst MAC + src MAC + ethertype +
     * payload already assembled by the caller). Returns 1 if the
     * device accepted it for transmission, 0 otherwise. This says
     * nothing about whether it reached anywhere - same honesty rule
     * as the rest of the driver stack: "accepted" is all any of these
     * layers can promise without an ACK from the layer above. */
    int (*tx)(const uint8_t* frame, uint32_t len);
};

struct netdev {
    uint8_t mac[6];
    uint8_t ip[4];         /* 0.0.0.0 until DHCP or static config runs */
    uint8_t gateway_ip[4];
    uint8_t netmask[4];
    int up;                /* 1 once link-layer association would be
                               complete; see the honesty note above -
                               nothing in this tree can set this to a
                               real "associated to an AP" state yet */
    const struct netdev_ops* ops;
};

/* There is exactly one netdev in this kernel; multi-interface support
 * is not a problem VoidOS has yet. */
struct netdev* netdev_get(void);

void netdev_register(const uint8_t mac[6], const struct netdev_ops* ops);

/* Called by the ops->tx backing implementation's RX side (interrupt
 * handler, poll loop, whatever) whenever a full Ethernet-II frame has
 * arrived. Dispatches by ethertype to arp_handle_frame() / ip_handle_frame().
 * Nothing in this tree currently calls this in response to a real
 * received frame - see the note above. */
void netdev_rx(const uint8_t* frame, uint32_t len);

int netdev_tx(const uint8_t dst_mac[6], uint16_t ethertype,
              const uint8_t* payload, uint32_t len);

/* Registers the honest-null backing device described above. Call once
 * at boot before any protocol layer tries to send anything. */
void netdev_null_init(void);

#endif
