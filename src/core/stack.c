#include "core/stack.h"
#include "netdev/netdev.h"

#include <string.h>

void pf_stack_init(struct pf_stack *s)
{
    memset(s, 0, sizeof(*s));
    arp_init(&s->arp);
    ip_reass_init(&s->reass);
    pf_socktab_init(&s->socks);
    tcp_init(&s->tcp);
}

void pf_stack_fini(struct pf_stack *s)
{
    for (size_t i = 0; i < ARP_CACHE_SIZE; i++)
        pktq_free_all(&s->arp.entries[i].waitq);
    ip_reass_fini(&s->reass);
    pf_socktab_fini(&s->socks);
    tcp_fini(&s->tcp);
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
    arp_tick(s, now_ms);
    ip_reass_tick(s, now_ms);
    tcp_tick(s, now_ms);
}
