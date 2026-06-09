/*
 * pfstack — the PacketForge stack daemon.
 *
 * Runs the stack on one or more TAP interfaces: answers ARP/ICMP, hosts
 * the socket layer; later phases add router mode (forwarding + CLI).
 *
 * Usage:
 *   pfstack --if pf0,10.190.0.2/24 [--if pf1,...] [--hexdump] [--verbose]
 */
#include "app_common.h"

#include <getopt.h>

int main(int argc, char **argv)
{
    static struct option opts[] = {
        {"if", required_argument, NULL, 'i'},
        {"hexdump", no_argument, NULL, 'x'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };

    struct pf_stack stack;
    pf_stack_init(&stack);
    stack.poll_fn = pf_loop_once;

    int c;
    int n_if = 0;
    while ((c = getopt_long(argc, argv, "i:xvh", opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            if (app_add_iface(&stack, optarg) < 0)
                return 1;
            n_if++;
            break;
        case 'x':
            stack.hexdump_rx = true;
            break;
        case 'v':
            pf_log_threshold = PF_LOG_DEBUG;
            break;
        case 'h':
        default:
            fprintf(stderr, "usage: %s --if NAME[,A.B.C.D/PLEN] [--if ...] [--hexdump]\n", argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }
    if (n_if == 0) {
        fprintf(stderr, "usage: %s --if NAME[,A.B.C.D/PLEN] [--if ...] [--hexdump]\n", argv[0]);
        return 1;
    }

    app_install_signals();
    printf("pfstack: ready (%d interface%s)\n", n_if, n_if == 1 ? "" : "s");
    fflush(stdout);

    while (!g_stop) {
        if (pf_loop_once(&stack, 10) < 0)
            break;
        app_poll_signals(&stack);
    }

    PF_INFO("pfstack: shutting down");
    app_shutdown(&stack);
    return 0;
}
