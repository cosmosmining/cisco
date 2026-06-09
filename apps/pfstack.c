/*
 * pfstack — the PacketForge stack daemon / router.
 *
 * Host mode: answers ARP/ICMP on its interfaces.
 * Router mode (--forward): forwards between interfaces with an LPM FIB,
 * IOS-style CLI over a UNIX socket (--cli PATH).
 *
 * Usage:
 *   pfstack --if rt0,10.191.1.2/24 --if rt1,10.191.2.2/24 \
 *           [--forward] [--cli /tmp/pf-cli.sock] \
 *           [--route 10.99.0.0/24,10.191.1.1] [--gw A.B.C.D] [--hexdump]
 */
#include "app_common.h"
#include "cli/cli.h"
#include "route/fib.h"

#include <getopt.h>

#define MAX_PENDING_ROUTES 16

int main(int argc, char **argv)
{
    static struct option opts[] = {
        {"if", required_argument, NULL, 'i'},
        {"forward", no_argument, NULL, 'f'},
        {"cli", required_argument, NULL, 'c'},
        {"route", required_argument, NULL, 'r'},
        {"gw", required_argument, NULL, 'g'},
        {"hexdump", no_argument, NULL, 'x'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0},
    };
    static const char usage[] =
        "usage: pfstack --if NAME[,A.B.C.D/PLEN] [--if ...] [--forward]\n"
        "               [--cli PATH] [--route P/L,NH]... [--gw IP] [--hexdump]\n";

    struct pf_stack stack;
    pf_stack_init(&stack);
    stack.poll_fn = pf_loop_once;

    char *pending_routes[MAX_PENDING_ROUTES];
    int n_routes = 0;
    const char *cli_path = NULL;
    int c;
    int n_if = 0;
    while ((c = getopt_long(argc, argv, "i:fc:r:g:xvh", opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            if (app_add_iface(&stack, optarg) < 0)
                return 1;
            n_if++;
            break;
        case 'f':
            stack.forwarding = true;
            break;
        case 'c':
            cli_path = optarg;
            break;
        case 'r':
            if (n_routes < MAX_PENDING_ROUTES)
                pending_routes[n_routes++] = optarg;
            break;
        case 'g':
            if (app_set_gw(&stack, optarg) < 0)
                return 1;
            break;
        case 'x':
            stack.hexdump_rx = true;
            break;
        case 'v':
            pf_log_threshold = PF_LOG_DEBUG;
            break;
        case 'h':
        default:
            fputs(usage, stderr);
            return c == 'h' ? 0 : 1;
        }
    }
    if (n_if == 0) {
        fputs(usage, stderr);
        return 1;
    }

    if (app_apply_gw(&stack) < 0)
        return 1;

    /* Static routes ("P/L,NH") install after connected routes exist. */
    for (int i = 0; i < n_routes; i++) {
        char *spec = pending_routes[i];
        char *slash = strchr(spec, '/');
        char *comma = slash ? strchr(slash, ',') : NULL;
        uint32_t prefix, nh;
        if (!slash || !comma) {
            fprintf(stderr, "bad route %s (want P/L,NH)\n", spec);
            return 1;
        }
        *slash = *comma = '\0';
        int plen = atoi(slash + 1);
        if (!pf_ip4_parse(spec, &prefix) || !pf_ip4_parse(comma + 1, &nh) || plen < 0 ||
            plen > 32 || fib_add_via(&stack, prefix, (uint8_t)plen, nh) < 0) {
            fprintf(stderr, "cannot install route %s/%d\n", spec, plen);
            return 1;
        }
    }

    if (cli_path && cli_open(&stack, cli_path) < 0)
        return 1;

    app_install_signals();
    printf("pfstack: ready (%d interface%s%s)\n", n_if, n_if == 1 ? "" : "s",
           stack.forwarding ? ", forwarding" : "");
    fflush(stdout);

    while (!g_stop) {
        if (pf_loop_once(&stack, 10) < 0)
            break;
        app_poll_signals(&stack);
    }

    PF_INFO("pfstack: shutting down");
    cli_close(&stack);
    app_shutdown(&stack);
    return 0;
}
