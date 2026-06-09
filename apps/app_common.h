/*
 * Shared plumbing for PacketForge apps: --if parsing, signal handling,
 * SIGUSR1 stats dump. Header-only since each app is a single source file.
 */
#ifndef PF_APPS_APP_COMMON_H
#define PF_APPS_APP_COMMON_H

#include "core/stack.h"
#include "netdev/netdev.h"
#include "netdev/tap_linux.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop; /* signal flags, see DECISIONS.md D-007 */
static volatile sig_atomic_t g_dump;

static void app_on_signal(int sig)
{
    if (sig == SIGUSR1)
        g_dump = 1;
    else
        g_stop = 1;
}

static void app_install_signals(void)
{
    signal(SIGINT, app_on_signal);
    signal(SIGTERM, app_on_signal);
    signal(SIGUSR1, app_on_signal);
}

/* SIGUSR1 → dump all counters in a machine-parsable form (tests use this
 * to assert that every malformed input lands in a drop counter). */
static void app_dump_stats(const struct pf_stack *stack)
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

/* Service pending SIGUSR1; call from app loops between blocking calls. */
static void app_poll_signals(const struct pf_stack *stack)
{
    if (g_dump) {
        g_dump = 0;
        app_dump_stats(stack);
    }
}

/* Parse "NAME[,A.B.C.D/PLEN]" and attach the interface. */
static int app_add_iface(struct pf_stack *stack, char *spec)
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

/* --gw is recorded during option parsing and installed as the default
 * route after all interfaces (and their connected routes) exist. */
static uint32_t app_pending_gw;

__attribute__((unused)) static int app_set_gw(struct pf_stack *stack, const char *arg)
{
    (void)stack;
    if (!pf_ip4_parse(arg, &app_pending_gw)) {
        fprintf(stderr, "bad gateway %s\n", arg);
        return -1;
    }
    return 0;
}

__attribute__((unused)) static int app_apply_gw(struct pf_stack *stack)
{
    if (app_pending_gw == 0)
        return 0;
    if (fib_add_via(stack, 0, 0, app_pending_gw) < 0) {
        fprintf(stderr, "gateway not reachable via a connected route\n");
        return -1;
    }
    return 0;
}

static void app_shutdown(struct pf_stack *stack)
{
    pf_stack_fini(stack);
    for (int i = 0; i < stack->ndevs; i++)
        pf_tap_close(stack->devs[i]);
}

#endif /* PF_APPS_APP_COMMON_H */
