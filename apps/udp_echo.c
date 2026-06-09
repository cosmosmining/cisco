/*
 * udp_echo — RFC 862 echo service on the PacketForge stack.
 *
 * Usage: udp_echo --if pf0,10.190.0.2/24 [--port 7]
 */
#include "app_common.h"
#include "udp/sock.h"

#include <getopt.h>

int main(int argc, char **argv)
{
    static struct option opts[] = {
        {"if", required_argument, NULL, 'i'},
        {"port", required_argument, NULL, 'p'},
        {"verbose", no_argument, NULL, 'v'},
        {0, 0, 0, 0},
    };

    struct pf_stack stack;
    pf_stack_init(&stack);
    stack.poll_fn = pf_loop_once;

    int n_if = 0;
    uint16_t port = 7;
    int c;
    while ((c = getopt_long(argc, argv, "i:p:v", opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            if (app_add_iface(&stack, optarg) < 0)
                return 1;
            n_if++;
            break;
        case 'p':
            port = (uint16_t)atoi(optarg);
            break;
        case 'v':
            pf_log_threshold = PF_LOG_DEBUG;
            break;
        default:
            fprintf(stderr, "usage: %s --if NAME,A.B.C.D/PLEN [--port N]\n", argv[0]);
            return 1;
        }
    }
    if (n_if == 0) {
        fprintf(stderr, "usage: %s --if NAME,A.B.C.D/PLEN [--port N]\n", argv[0]);
        return 1;
    }

    int sock = pf_socket(&stack, PF_SOCK_UDP);
    if (sock < 0 || pf_bind(&stack, sock, 0, port) < 0) {
        fprintf(stderr, "bind %u failed\n", port);
        return 1;
    }

    app_install_signals();
    printf("udp_echo: ready on port %u\n", port);
    fflush(stdout);

    static uint8_t buf[PKT_BUF_SIZE];
    while (!g_stop) {
        uint32_t src_ip;
        uint16_t src_port;
        long n = pf_recvfrom(&stack, sock, buf, sizeof(buf), &src_ip, &src_port, 200);
        app_poll_signals(&stack);
        if (n <= 0)
            continue;
        pf_sendto(&stack, sock, buf, (size_t)n, src_ip, src_port);
    }

    pf_close(&stack, sock);
    app_shutdown(&stack);
    return 0;
}
