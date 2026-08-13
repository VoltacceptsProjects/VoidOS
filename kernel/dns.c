#include "dns.h"
#include "udp.h"
#include "serial.h"

#define DNS_PORT 53
#define DNS_LOCAL_PORT 51234
#define DNS_MAX_MSG 512
#define DNS_POLL_RETRIES 200000 /* busy-poll iterations, not real time - see tcp.c/udp.h */

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

static uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static uint32_t encode_qname(const char* hostname, uint8_t* out, uint32_t out_cap) {
    /* "www.example.com" -> 03 www 07 example 03 com 00 */
    uint32_t out_pos = 0;
    uint32_t label_start = 0;
    uint32_t i = 0;
    for (;;) {
        char c = hostname[i];
        if (c == '.' || c == '\0') {
            uint32_t label_len = i - label_start;
            if (label_len == 0 || label_len > 63) return 0;
            if (out_pos + 1 + label_len >= out_cap) return 0;
            out[out_pos++] = (uint8_t)label_len;
            for (uint32_t j = 0; j < label_len; j++) out[out_pos++] = (uint8_t)hostname[label_start + j];
            label_start = i + 1;
            if (c == '\0') break;
        }
        i++;
    }
    if (out_pos >= out_cap) return 0;
    out[out_pos++] = 0; /* root label */
    return out_pos;
}

int dns_resolve(const uint8_t dns_server_ip[4], const char* hostname, uint8_t out_ip[4]) {
    uint8_t query[DNS_MAX_MSG];
    struct dns_header* qhdr = (struct dns_header*)query;

    static uint16_t txn_id = 0x1234;
    txn_id++;

    qhdr->id = be16(txn_id);
    qhdr->flags = be16(0x0100); /* standard query, recursion desired */
    qhdr->qdcount = be16(1);
    qhdr->ancount = 0;
    qhdr->nscount = 0;
    qhdr->arcount = 0;

    uint32_t pos = sizeof(struct dns_header);
    uint32_t qname_len = encode_qname(hostname, query + pos, DNS_MAX_MSG - pos);
    if (qname_len == 0) {
        serial_writestring("[dns] hostname too long/malformed for the query buffer\n");
        return 0;
    }
    pos += qname_len;
    if (pos + 4 > DNS_MAX_MSG) return 0;
    query[pos++] = 0x00; query[pos++] = 0x01; /* QTYPE = A */
    query[pos++] = 0x00; query[pos++] = 0x01; /* QCLASS = IN */

    serial_writestring("[dns] querying for ");
    serial_writestring(hostname);
    serial_writestring("\n");

    if (!udp_send(dns_server_ip, DNS_LOCAL_PORT, DNS_PORT, query, pos)) {
        serial_writestring("[dns] udp_send failed (no ARP entry for resolver yet, "
                            "or send path otherwise not ready)\n");
        return 0;
    }

    uint8_t response[DNS_MAX_MSG];
    uint32_t resp_len = 0;
    for (uint32_t tries = 0; tries < DNS_POLL_RETRIES; tries++) {
        resp_len = udp_poll_recv(DNS_LOCAL_PORT, response, DNS_MAX_MSG);
        if (resp_len > 0) break;
    }

    if (resp_len == 0) {
        serial_writestring("[dns] no response (expected until a real 802.11 RX "
                            "datapath exists - see netdev.h)\n");
        return 0;
    }

    if (resp_len < sizeof(struct dns_header)) return 0;
    const struct dns_header* rhdr = (const struct dns_header*)response;
    if (be16(rhdr->id) != txn_id) return 0;
    uint16_t flags = be16(rhdr->flags);
    if (!(flags & 0x8000)) return 0; /* not a response */
    if ((flags & 0x000F) != 0) {
        serial_writestring("[dns] server returned an error rcode\n");
        return 0;
    }

    uint16_t qdcount = be16(rhdr->qdcount);
    uint16_t ancount = be16(rhdr->ancount);
    uint32_t p = sizeof(struct dns_header);

    /* Skip the question section we sent back (server echoes it). */
    for (uint16_t q = 0; q < qdcount; q++) {
        while (p < resp_len && response[p] != 0) {
            if ((response[p] & 0xC0) == 0xC0) { p += 2; goto qdone; } /* compressed name, shouldn't appear in Q section but be safe */
            p += response[p] + 1;
        }
        p += 1; /* the zero root label */
qdone:
        p += 4; /* QTYPE + QCLASS */
    }

    for (uint16_t a = 0; a < ancount; a++) {
        if (p >= resp_len) return 0;
        /* NAME: either a label sequence or a compression pointer (0xC0 xx) - either way, skip it */
        if ((response[p] & 0xC0) == 0xC0) {
            p += 2;
        } else {
            while (p < resp_len && response[p] != 0) p += response[p] + 1;
            p += 1;
        }
        if (p + 10 > resp_len) return 0;
        uint16_t type = be16((uint16_t)((response[p] << 8) | response[p + 1]));
        uint16_t rdlength = be16((uint16_t)((response[p + 8] << 8) | response[p + 9]));
        p += 10;
        if (p + rdlength > resp_len) return 0;

        if (type == 1 && rdlength == 4) { /* A record */
            for (int i = 0; i < 4; i++) out_ip[i] = response[p + i];
            serial_writestring("[dns] resolved\n");
            return 1;
        }
        p += rdlength;
    }

    serial_writestring("[dns] response had no A record\n");
    return 0;
}
