/*
 * pfstack — the PacketForge stack daemon.
 *
 * Phase 0: opens TAP device(s) and hexdumps received frames.
 * Later phases grow this into the full host stack / router.
 *
 * Usage:
 *   pfstack --if pf0,192.0.2.2/24 [--if pf1,...] [--hexdump] [--verbose]
 */
#include "core/stack.h"
#include "netdev/tap_linux.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop; /* signal flags, see DECISIONS.md D-007 */
static volatile sig_atomic_t g_dump;

static void on_signal(int sig)
{
    if (sig == SIGUSR1)
        g_dump = 1;
    else
        g_stop = 1;
}

/* SIGUSR1 → dump all counters in a machine-parsable form (tests use this
 * to assert that every malformed input lands in a drop counter). */
static void dump_stats(const struct pf_stack *stack)
{
    printf("--- stats ---\n");
    for (size_t i = 0; i < pf_stats_count(); i++)
        printf("stat %s %llu\n", pf_stats_name(i),
               (unsigned long long)pf_stats_get(&stack->stats, i));
    for (int i = 0; i < stack->ndevs; i++) {
        const struct netdev *d = stack->devs[i];
        printf("ifstat %s rx_pkts %llu rx_bytes %llu rx_drops %llu tx_pkts %llu tx_bytes %llu "
               "tx_errs %llu\n",
               d->name, (unsigned long long)d->st.rx_pkts, (unsigned long long)d->st.rx_bytes,
               (unsigned long long)d->st.rx_drops, (unsigned long long)d->st.tx_pkts,
               (unsigned long long)d->st.tx_bytes, (unsigned long long)d->st.tx_errs);
    }
    printf("--- end stats ---\n");
    fflush(stdout);
}

/* Parse "NAME[,A.B.C.D/PLEN]" and attach the interface. */
static int add_iface(struct pf_stack *stack, char *spec)
{
    char *addr = strchr(spec, ',');
    if (addr)
        *addr++ = '\0';

    struct netdev *dev = pf_tap_open(stack, spec);
    if (!dev)
        return -1;

    if (addr) {
        char *slash = strchr(addr, '/');
        if (!slash) {
            fprintf(stderr, "bad address %s (want A.B.C.D/PLEN)\n", addr);
            return -1;
        }
        *slash++ = '\0';
        uint32_t ip;
        int plen = atoi(slash);
        if (!pf_ip4_parse(addr, &ip) || plen < 0 || plen > 32) {
            fprintf(stderr, "bad address %s/%s\n", addr, slash);
            return -1;
        }
        uint32_t mask = plen == 0 ? 0 : 0xffffffffu << (32 - plen);
        pf_if_set_addr(stack, dev, ip, mask);
        char ips[16];
        PF_INFO("%s: address %s/%d", dev->name, pf_ip4_str(ip, ips), plen);
    }
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --if NAME[,A.B.C.D/PLEN] [--if ...] [--hexdump] [--verbose]\n",
            argv0);
}

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

    int c;
    int n_if = 0;
    while ((c = getopt_long(argc, argv, "i:xvh", opts, NULL)) != -1) {
        switch (c) {
        case 'i':
            if (add_iface(&stack, optarg) < 0)
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
            usage(argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }
    if (n_if == 0) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);

    printf("pfstack: ready (%d interface%s)\n", n_if, n_if == 1 ? "" : "s");
    fflush(stdout);

    while (!g_stop) {
        if (pf_loop_once(&stack, 10) < 0)
            break;
        if (g_dump) {
            g_dump = 0;
            dump_stats(&stack);
        }
    }

    PF_INFO("pfstack: shutting down");
    pf_stack_fini(&stack);
    for (int i = 0; i < stack.ndevs; i++)
        pf_tap_close(stack.devs[i]);
    return 0;
}
