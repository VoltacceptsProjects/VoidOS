#include "dhcp.h"
#include "udp.h"
#include "netdev.h"
#include "serial.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
/* sizeof(struct dhcp_packet) below, rounded up - keep this buffer at
 * least that big so the cast in wait_for_reply() never reads past it.
 * A real server reply may carry more options than our fixed 64-byte
 * options[] field holds (lease time, domain name, etc.) - those extra
 * bytes are simply not looked at; find_option() only scans within
 * options[], which is a real (if narrow) limitation worth stating
 * plainly rather than silently. */
#define DHCP_MSG_SIZE 320
#define DHCP_POLL_RETRIES 200000 /* busy-poll iterations - see tcp.c note on why this isn't real time */
#define DHCP_MAGIC_COOKIE 0x63825363u

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5

struct dhcp_packet {
    uint8_t  op;      /* 1 = BOOTREQUEST, 2 = BOOTREPLY */
    uint8_t  htype;   /* 1 = Ethernet */
    uint8_t  hlen;    /* 6 */
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
    uint8_t  options[64];
} __attribute__((packed));

static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint32_t be32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF);
}

static void fill_common(struct dhcp_packet* pkt, uint32_t xid, const uint8_t mac[6]) {
    for (uint32_t i = 0; i < sizeof(*pkt); i++) ((uint8_t*)pkt)[i] = 0;
    pkt->op = 1;
    pkt->htype = 1;
    pkt->hlen = 6;
    pkt->hops = 0;
    pkt->xid = be32(xid);
    pkt->secs = 0;
    pkt->flags = be16(0x8000); /* ask for a broadcast reply - we have no IP to receive unicast on yet */
    for (int i = 0; i < 6; i++) pkt->chaddr[i] = mac[i];
    pkt->magic_cookie = be32(DHCP_MAGIC_COOKIE);
}

static uint32_t build_discover(struct dhcp_packet* pkt, uint32_t xid, const uint8_t mac[6]) {
    fill_common(pkt, xid, mac);
    uint32_t o = 0;
    pkt->options[o++] = 53; pkt->options[o++] = 1; pkt->options[o++] = DHCPDISCOVER; /* message type */
    pkt->options[o++] = 55; pkt->options[o++] = 3; /* parameter request list */
    pkt->options[o++] = 1;  /* subnet mask */
    pkt->options[o++] = 3;  /* router */
    pkt->options[o++] = 6;  /* DNS server */
    pkt->options[o++] = 255; /* end */
    return (uint32_t)((uint8_t*)&pkt->options[o] - (uint8_t*)pkt);
}

static uint32_t build_request(struct dhcp_packet* pkt, uint32_t xid, const uint8_t mac[6],
                               const uint8_t requested_ip[4], const uint8_t server_ip[4]) {
    fill_common(pkt, xid, mac);
    uint32_t o = 0;
    pkt->options[o++] = 53; pkt->options[o++] = 1; pkt->options[o++] = DHCPREQUEST;
    pkt->options[o++] = 50; pkt->options[o++] = 4; /* requested IP */
    for (int i = 0; i < 4; i++) pkt->options[o++] = requested_ip[i];
    pkt->options[o++] = 54; pkt->options[o++] = 4; /* server identifier */
    for (int i = 0; i < 4; i++) pkt->options[o++] = server_ip[i];
    pkt->options[o++] = 255;
    return (uint32_t)((uint8_t*)&pkt->options[o] - (uint8_t*)pkt);
}

/* Scans the options area (which, for a well-formed reply, spills past
 * the fixed 64-byte options[] field in this struct - but this kernel
 * only ever sends a request that fits well inside a small reply, and
 * this parser stops at whichever comes first: an End option or the
 * bound it was given) for a given option code. Returns 1 and fills
 * out (up to out_cap) if found. */
static int find_option(const uint8_t* options, uint32_t len, uint8_t code,
                        uint8_t* out, uint32_t out_cap) {
    uint32_t p = 0;
    while (p < len) {
        uint8_t opt = options[p];
        if (opt == 255) break; /* End */
        if (opt == 0) { p++; continue; } /* Pad */
        if (p + 1 >= len) break;
        uint8_t optlen = options[p + 1];
        if (p + 2 + optlen > len) break;
        if (opt == code) {
            uint32_t n = optlen < out_cap ? optlen : out_cap;
            for (uint32_t i = 0; i < n; i++) out[i] = options[p + 2 + i];
            return 1;
        }
        p += 2 + optlen;
    }
    return 0;
}

static int wait_for_reply(struct dhcp_packet* out_pkt, uint32_t xid) {
    uint8_t buf[DHCP_MSG_SIZE];
    for (uint32_t tries = 0; tries < DHCP_POLL_RETRIES; tries++) {
        uint32_t n = udp_poll_recv(DHCP_CLIENT_PORT, buf, sizeof(buf));
        if (n == 0) continue;
        if (n < sizeof(struct dhcp_packet) - sizeof(out_pkt->options) + 4) continue; /* too short to have even the cookie */
        struct dhcp_packet* pkt = (struct dhcp_packet*)buf;
        if (pkt->op != 2) continue; /* not a BOOTREPLY */
        if (be32(pkt->xid) != xid) continue;
        if (be32(pkt->magic_cookie) != DHCP_MAGIC_COOKIE) continue;
        *out_pkt = *pkt;
        return 1;
    }
    return 0;
}

int dhcp_request(struct dhcp_config* out) {
    struct netdev* dev = netdev_get();
    if (!dev) {
        serial_writestring("[dhcp] no netdev registered\n");
        return 0;
    }

    uint8_t broadcast_ip[4] = { 255, 255, 255, 255 };
    uint32_t xid = 0xD8C7B6A5u; /* fixed is fine - one DHCP exchange per boot, no collision risk to guard against */

    struct dhcp_packet discover;
    uint32_t discover_len = build_discover(&discover, xid, dev->mac);

    serial_writestring("[dhcp] sending DISCOVER\n");
    if (!udp_send(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                   (const uint8_t*)&discover, discover_len)) {
        serial_writestring("[dhcp] udp_send(DISCOVER) failed\n");
        return 0;
    }

    struct dhcp_packet offer;
    if (!wait_for_reply(&offer, xid)) {
        serial_writestring("[dhcp] no OFFER received (expected until a real "
                            "802.11 RX datapath exists - see netdev.h)\n");
        return 0;
    }

    uint8_t msgtype = 0;
    find_option(offer.options, sizeof(offer.options), 53, &msgtype, 1);
    if (msgtype != DHCPOFFER) {
        serial_writestring("[dhcp] reply wasn't an OFFER, giving up\n");
        return 0;
    }

    uint8_t offered_ip[4];
    for (int i = 0; i < 4; i++) offered_ip[i] = offer.yiaddr[i];
    uint8_t server_id[4] = { 0, 0, 0, 0 };
    find_option(offer.options, sizeof(offer.options), 54, server_id, 4);

    serial_writestring("[dhcp] got OFFER, sending REQUEST\n");
    struct dhcp_packet request;
    uint32_t request_len = build_request(&request, xid, dev->mac, offered_ip, server_id);
    if (!udp_send(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                   (const uint8_t*)&request, request_len)) {
        serial_writestring("[dhcp] udp_send(REQUEST) failed\n");
        return 0;
    }

    struct dhcp_packet ack;
    if (!wait_for_reply(&ack, xid)) {
        serial_writestring("[dhcp] no ACK received\n");
        return 0;
    }
    msgtype = 0;
    find_option(ack.options, sizeof(ack.options), 53, &msgtype, 1);
    if (msgtype != DHCPACK) {
        serial_writestring("[dhcp] server did not ACK the request\n");
        return 0;
    }

    for (int i = 0; i < 4; i++) out->ip[i] = ack.yiaddr[i];
    uint8_t netmask[4] = { 255, 255, 255, 0 }; /* sane default if option 1 is absent */
    find_option(ack.options, sizeof(ack.options), 1, netmask, 4);
    for (int i = 0; i < 4; i++) out->netmask[i] = netmask[i];

    uint8_t gateway[4] = { 0, 0, 0, 0 };
    find_option(ack.options, sizeof(ack.options), 3, gateway, 4);
    for (int i = 0; i < 4; i++) out->gateway[i] = gateway[i];

    uint8_t dns[4] = { 0, 0, 0, 0 };
    find_option(ack.options, sizeof(ack.options), 6, dns, 4);
    for (int i = 0; i < 4; i++) out->dns_server[i] = dns[i];

    serial_writestring("[dhcp] bound\n");
    return 1;
}
