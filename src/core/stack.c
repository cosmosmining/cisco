#include "core/stack.h"
#include "netdev/netdev.h"

#include <string.h>

void pf_stack_init(struct pf_stack *s)
{
    memset(s, 0, sizeof(*s));
}

int pf_stack_add_dev(struct pf_stack *s, struct netdev *dev)
{
    if (s->ndevs >= PF_MAX_DEVS)
        return -1;
    dev->ifindex = s->ndevs;
    dev->stack = s;
    s->devs[s->ndevs++] = dev;
    return dev->ifindex;
}

void pf_if_set_addr(struct pf_stack *s, struct netdev *dev, uint32_t ip, uint32_t mask)
{
    (void)s;
    dev->ip = ip;
    dev->mask = mask;
}

void pf_tick(struct pf_stack *s, uint64_t now_ms)
{
    (void)s;
    (void)now_ms;
    /* Module tick hooks land here as phases add timed state
     * (ARP aging, IP reassembly timeout, TCP timers). */
}
