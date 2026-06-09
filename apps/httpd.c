/*
 * httpd — a tiny HTTP/1.0 server on the PacketForge stack.
 *
 * GET /      → static landing page
 * GET /blob  → 1 MiB of deterministic pseudo-random bytes (transfer tests)
 *
 * Usage: httpd --if pf0,10.190.0.2/24 [--port 80] [--gw A.B.C.D]
 */
#include "app_common.h"
#include "udp/sock.h"

#include <getopt.h>

static const char PAGE[] = "<!doctype html>\n"
                           "<html><head><title>PacketForge</title></head>\n"
                           "<body><h1>PacketForge</h1>\n"
                           "<p>This page was served by a from-scratch TCP/IP stack in C:\n"
                           "Ethernet, ARP, IPv4, ICMP, UDP and TCP over a Linux TAP device —\n"
                           "no kernel sockets, no lwIP.</p>\n"
                           "</body></html>\n";

#define BLOB_LEN ((size_t)1024 * 1024)

/* xorshift32 — deterministic blob so clients can hash-verify transfers. */
static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void serve_client(struct pf_stack *stack, int cs)
{
    static uint8_t req[2048];
    size_t got = 0;

    /* Read until the end of the request head (or 5 s of silence). */
    while (got < sizeof(req) - 1) {
        long n = pf_recv(stack, cs, req + got, sizeof(req) - 1 - got, 5000);
        if (n <= 0)
            return;
        got += (size_t)n;
        req[got] = '\0';
        if (strstr((char *)req, "\r\n\r\n") || strstr((char *)req, "\n\n"))
            break;
    }

    bool want_blob = strncmp((char *)req, "GET /blob", 9) == 0;
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.0 200 OK\r\n"
                      "Server: packetforge-httpd\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %u\r\n"
                      "Connection: close\r\n\r\n",
                      want_blob ? "application/octet-stream" : "text/html",
                      want_blob ? (unsigned)BLOB_LEN : (unsigned)(sizeof(PAGE) - 1));
    if (pf_send(stack, cs, hdr, (size_t)hl) < 0)
        return;

    if (!want_blob) {
        pf_send(stack, cs, PAGE, sizeof(PAGE) - 1);
        return;
    }

    static uint8_t chunk[8192];
    uint32_t seed = 0x9e3779b9;
    size_t left = BLOB_LEN;
    while (left > 0) {
        size_t n = PF_MIN(left, sizeof(chunk));
        for (size_t i = 0; i < n; i += 4) {
            uint32_t v = xs32(&seed);
            memcpy(chunk + i, &v, PF_MIN((size_t)4, n - i));
        }
        if (pf_send(stack, cs, chunk, n) < 0)
            return;
        left -= n;
    }
}

int main(int argc, char **argv)
{
    static struct option opts[] = {
        {"if", required_argument, NULL, 'i'},
        {"port", required_argument, NULL, 'p'},
        {"gw", required_argument, NULL, 'g'},
        {"verbose", no_argument, NULL, 'v'},
        {0, 0, 0, 0},
    };

    struct pf_stack stack;
    pf_stack_init(&stack);
    stack.poll_fn = pf_loop_once;

    int n_if = 0;
    uint16_t port = 80;
    int c;
    while ((c = getopt_long(argc, argv, "i:p:g:v", opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            if (app_add_iface(&stack, optarg) < 0)
                return 1;
            n_if++;
            break;
        case 'p':
            port = (uint16_t)atoi(optarg);
            break;
        case 'g':
            if (app_set_gw(&stack, optarg) < 0)
                return 1;
            break;
        case 'v':
            pf_log_threshold = PF_LOG_DEBUG;
            break;
        default:
            fprintf(stderr, "usage: %s --if NAME,A.B.C.D/PLEN [--port N] [--gw IP]\n", argv[0]);
            return 1;
        }
    }
    if (n_if == 0) {
        fprintf(stderr, "usage: %s --if NAME,A.B.C.D/PLEN [--port N] [--gw IP]\n", argv[0]);
        return 1;
    }

    int lsock = pf_socket(&stack, PF_SOCK_TCP);
    if (lsock < 0 || pf_bind(&stack, lsock, 0, port) < 0 || pf_listen(&stack, lsock, 4) < 0) {
        fprintf(stderr, "listen on %u failed\n", port);
        return 1;
    }

    app_install_signals();
    printf("httpd: ready on port %u\n", port);
    fflush(stdout);

    while (!g_stop) {
        app_poll_signals(&stack);
        int cs = pf_accept(&stack, lsock, NULL, NULL, 200);
        if (cs < 0)
            continue;
        serve_client(&stack, cs);
        pf_close(&stack, cs); /* HTTP/1.0: close after each response */
    }

    pf_close(&stack, lsock);
    app_shutdown(&stack);
    return 0;
}
