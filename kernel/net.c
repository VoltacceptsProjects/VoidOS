#include "net.h"
#include "multiboot.h"
#include "serial.h"
#include "netdev.h"
#include "arp.h"
#include "ip.h"
#include "udp.h"
#include "tcp.h"
#include "dhcp.h"
#include "dns.h"
#include "io.h"

static int g_net_configured = 0;
static uint8_t g_dns_server[4];

/* Same busy-wait idiom as iwlwifi.c's short_delay() - see that file's
 * comment for why inb(0x80) is used as a crude time-burner in a
 * kernel with no timer driver yet. */
static void net_short_delay(uint32_t iterations) {
    for (uint32_t i = 0; i < iterations; i++) inb(0x80);
}

void net_init(void) {
    netdev_null_init();
    arp_init();
    udp_init();
    tcp_init();

    struct dhcp_config cfg;
    serial_writestring("[net] attempting DHCP...\n");
    if (dhcp_request(&cfg)) {
        ip_init(cfg.ip, cfg.gateway, cfg.netmask);
        for (int i = 0; i < 4; i++) g_dns_server[i] = cfg.dns_server[i];
        g_net_configured = 1;
        serial_writestring("[net] configured via DHCP\n");
    } else {
        uint8_t zero[4] = { 0, 0, 0, 0 };
        ip_init(zero, zero, zero);
        g_net_configured = 0;
        serial_writestring("[net] DHCP did not complete - see dhcp.c. Falling "
                            "back to Multiboot-module staging for any "
                            "net_http_get() call for the rest of this boot.\n");
    }
}

/* Runs a real DNS -> TCP connect -> HTTP/1.0 GET -> parse-past-headers
 * attempt. Returns 1 and fills out/out_size on a clean 200 response,
 * 0 on any failure along the way (DNS timeout, connect timeout, non-
 * 200 status, or the response not completing before the retry budget
 * below runs out). Every one of those failure points is expected to
 * be hit today, for exactly the reason net.h states up front: nothing
 * in this kernel yet drives netdev_rx() with real received 802.11
 * frames, so no reply from any real host has ever landed here. This
 * function is nonetheless written to be correct against RFC 793/1035/
 * 2131/2616 for the day a real datapath exists under netdev - at that
 * point nothing here or in any of its callers needs to change. */
static int try_real_http_get(const char* host, const char* path,
                              uint8_t* out, uint32_t out_cap, uint32_t* out_size) {
    if (!g_net_configured) {
        serial_writestring("[net]   no DHCP config, skipping real HTTP attempt\n");
        return 0;
    }

    uint8_t server_ip[4];
    if (!dns_resolve(g_dns_server, host, server_ip)) {
        return 0;
    }

    tcp_handle_t h = tcp_connect(server_ip, 80);
    if (h < 0) return 0;

    uint32_t poll_iterations = 0;
    const uint32_t max_poll_iterations = 2000; /* bounded busy-wait budget, not real time */
    while (tcp_state(h) == TCP_SYN_SENT && poll_iterations < max_poll_iterations) {
        tcp_poll(h);
        net_short_delay(50);
        poll_iterations++;
    }
    if (tcp_state(h) != TCP_ESTABLISHED) {
        serial_writestring("[net]   TCP handshake never completed\n");
        return 0;
    }

    char request[256];
    uint32_t rp = 0;
    const char* parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host,
                             "\r\nConnection: close\r\nUser-Agent: VoidOS\r\n\r\n" };
    for (int part = 0; part < 5; part++) {
        const char* s = parts[part];
        while (*s && rp < sizeof(request) - 1) request[rp++] = *s++;
    }
    tcp_send(h, (const uint8_t*)request, rp);

    uint8_t response[4096];
    uint32_t response_len = 0;
    poll_iterations = 0;
    const uint32_t max_response_iterations = 4000;
    while (poll_iterations < max_response_iterations) {
        tcp_poll(h);
        uint32_t got = tcp_recv(h, response + response_len,
                                 (uint32_t)sizeof(response) - response_len);
        response_len += got;
        enum tcp_state st = tcp_state(h);
        if (st == TCP_CLOSE_WAIT || st == TCP_CLOSED) break;
        if (response_len >= sizeof(response)) break;
        net_short_delay(50);
        poll_iterations++;
    }
    tcp_close(h);

    if (response_len == 0) {
        serial_writestring("[net]   no response bytes ever arrived\n");
        return 0;
    }

    /* Find end of headers (CRLFCRLF) and check for "200" in the
     * status line - minimal HTTP/1.0 parsing, no chunked-encoding
     * support (HTTP/1.0 + Connection: close means the server is
     * expected to just close the connection when done, which is what
     * TCP_CLOSE_WAIT above is watching for). */
    uint32_t header_end = 0;
    for (uint32_t i = 0; i + 3 < response_len; i++) {
        if (response[i] == '\r' && response[i+1] == '\n' &&
            response[i+2] == '\r' && response[i+3] == '\n') {
            header_end = i + 4;
            break;
        }
    }
    if (header_end == 0) {
        serial_writestring("[net]   response had no complete header block\n");
        return 0;
    }
    int status_ok = (response_len > 11 &&
                      response[9] == '2' && response[10] == '0' && response[11] == '0');
    if (!status_ok) {
        serial_writestring("[net]   non-200 HTTP status\n");
        return 0;
    }

    uint32_t body_len = response_len - header_end;
    uint32_t copy_len = body_len < out_cap ? body_len : out_cap;
    for (uint32_t i = 0; i < copy_len; i++) out[i] = response[header_end + i];
    *out_size = copy_len;
    serial_writestring("[net]   got a real HTTP 200 response\n");
    return 1;
}

static int str_equal(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* path always arrives with a leading '/' (see net_http_get); module
 * names on the ISO don't have one, so this compares path+1 against the
 * module's filename portion of its Multiboot string (module strings can
 * carry a full "boot/foo.vapp"-style path depending on grub.cfg, so
 * this matches on the tail rather than requiring an exact match). */
static int name_matches(const char* module_string, const char* want_name) {
    if (!module_string || !want_name || !*want_name) return 0;

    int mlen = 0, wlen = 0;
    while (module_string[mlen]) mlen++;
    while (want_name[wlen]) wlen++;
    if (wlen > mlen) return 0;

    const char* tail = module_string + (mlen - wlen);
    if (!str_equal(tail, want_name)) return 0;
    /* Reject a false match like "notarealnotepad.vapp" matching
     * "notepad.vapp" - the char right before the tail must be a path
     * separator or nothing. */
    if (mlen == wlen) return 1;
    char before = module_string[mlen - wlen - 1];
    return before == '/' || before == '\\';
}

static int find_module_fallback(struct multiboot_info* mbi, const char* want_name,
                                 uint8_t* out, uint32_t out_cap, uint32_t* out_size) {
    if (!mbi || !mbi->mods_count || !mbi->mods_addr) return 0;
    const struct multiboot_module* modules =
        (const struct multiboot_module*)(uintptr_t)mbi->mods_addr;

    for (uint32_t i = 0; i < mbi->mods_count; i++) {
        const char* name = (const char*)(uintptr_t)modules[i].string;
        if (!name_matches(name, want_name)) continue;

        uint32_t size = modules[i].mod_end - modules[i].mod_start;
        uint32_t copy_len = size < out_cap ? size : out_cap;
        const uint8_t* data = (const uint8_t*)(uintptr_t)modules[i].mod_start;
        for (uint32_t b = 0; b < copy_len; b++) out[b] = data[b];

        *out_size = copy_len;
        serial_writestring("[net]   fallback: found Multiboot module \"");
        serial_writestring(name);
        serial_writestring("\" for \"");
        serial_writestring(want_name);
        serial_writestring("\"\n");
        if (copy_len < size) {
            serial_writestring("[net]   (truncated - out_cap smaller than the "
                                "module)\n");
        }
        return 1;
    }
    return 0;
}

int net_http_get(struct multiboot_info* mbi, const char* host, const char* path,
                  uint8_t* out, uint32_t out_cap, uint32_t* out_size) {
    *out_size = 0;

    serial_writestring("[net] GET ");
    serial_writestring(path ? path : "(null)");
    serial_writestring("\n");

    if (host && path && try_real_http_get(host, path, out, out_cap, out_size)) {
        return 1;
    }
    serial_writestring("[net]   real HTTP path did not produce a result - "
                        "see try_real_http_get()/net.h for why that's still "
                        "expected right now - falling back to the "
                        "Multiboot-module test fixture\n");

    const char* want = (path && path[0] == '/') ? path + 1 : path;
    if (find_module_fallback(mbi, want, out, out_cap, out_size)) {
        serial_writestring("[net]   (that came from an ISO-staged Multiboot "
                            "module, NOT the network - see net.h)\n");
        return 1;
    }

    serial_writestring("[net]   no matching Multiboot module staged either - "
                        "request fails\n");
    return 0;
}
