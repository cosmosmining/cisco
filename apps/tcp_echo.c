/*
 * tcp_echo — RFC 862 echo service over the PacketForge TCP stack.
 *
 * Usage: tcp_echo --if pf0,10.190.0.2/24 [--port 7] [--gw A.B.C.D]
 */
#include "app_common.h"
#include "udp/sock.h"

#include <getopt.h>

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
    uint16_t port = 7;
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

    if (app_apply_gw(&stack) < 0)
        return 1;

    int lsock = pf_socket(&stack, PF_SOCK_TCP);
    if (lsock < 0 || pf_bind(&stack, lsock, 0, port) < 0 || pf_listen(&stack, lsock, 4) < 0) {
        fprintf(stderr, "listen on %u failed\n", port);
        return 1;
    }

    app_install_signals();
    printf("tcp_echo: ready on port %u\n", port);
    fflush(stdout);

    static uint8_t buf[16384];
    while (!g_stop) {
        app_poll_signals(&stack);
        uint32_t rip;
        uint16_t rport;
        int cs = pf_accept(&stack, lsock, &rip, &rport, 200);
        if (cs < 0)
            continue;
        pf_sock_set_nodelay(&stack, cs, true); /* echo wants low latency */

        char ips[16];
        PF_INFO("tcp_echo: connection from %s:%u", pf_ip4_str(rip, ips), rport);
        while (!g_stop) {
            app_poll_signals(&stack);
            long n = pf_recv(&stack, cs, buf, sizeof(buf), 500);
            if (n == -2)
                continue; /* timeout — keep serving */
            if (n <= 0)
                break; /* EOF or error */
            if (pf_send(&stack, cs, buf, (size_t)n) < 0)
                break;
        }
        pf_close(&stack, cs);
        PF_INFO("tcp_echo: connection closed");
    }

    pf_close(&stack, lsock);
    app_shutdown(&stack);
    return 0;
}
