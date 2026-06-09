#include "arp/arp.h"
#include "core/stack.h"
#include "eth/eth.h"
#include "netdev/netdev.h"

#include <string.h>

void arp_init(struct arp_cache *c)
{
    memset(c, 0, sizeof(*c));
    for (size_t i = 0; i < ARP_CACHE_SIZE; i++)
        pktq_init(&c->entries[i].waitq);
}

struct arp_entry *arp_lookup(struct arp_cache *c, uint32_t ip)
{
    for (size_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (c->entries[i].state != ARP_FREE && c->entries[i].ip == ip)
            return &c->entries[i];
    }
    return NULL;
}

/* Free slot if available, else evict the entry closest to expiry. */
static struct arp_entry *arp_alloc_entry(struct pf_stack *stack)
{
    struct arp_cache *c = &stack->arp;
    struct arp_entry *victim = &c->entries[0];
    for (size_t i = 0; i < ARP_CACHE_SIZE; i++) {
        struct arp_entry *e = &c->entries[i];
        if (e->state == ARP_FREE)
            return e;
        if (e->deadline_ms < victim->deadline_ms)
            victim = e;
    }
    stack->stats.arp_cache_evictions++;
    pktq_free_all(&victim->waitq);
    memset(victim, 0, sizeof(*victim));
    pktq_init(&victim->waitq);
    return victim;
}

static void arp_entry_resolve(struct pf_stack *stack, struct arp_entry *e, struct netdev *dev,
                              const uint8_t mac[6], uint64_t now)
{
    bool was_pending = e->state == ARP_PENDING;
    e->state = ARP_RESOLVED;
    memcpy(e->mac, mac, 6);
    e->dev = dev;
    e->deadline_ms = now + ARP_CACHE_TTL_MS;
    e->tries = 0;

    if (was_pending) {
        /* RFC 1122 §2.3.2.2 — flush packets parked behind the resolution.
         * Only IPv4 ever queues here. */
        struct pkt *p;
        while ((p = pktq_pop(&e->waitq)) != NULL)
            eth_output(stack, dev, e->mac, ETH_TYPE_IP4, p);
    }
}

void arp_send_request(struct pf_stack *stack, struct netdev *dev, uint32_t target_ip)
{
    struct pkt *p = pkt_alloc();
    pkt_reserve(p, PKT_TX_HEADROOM);
    struct arp_hdr *ah = pkt_put(p, ARP_HDR_LEN);

    ah->htype = pf_htons(1); /* Ethernet */
    ah->ptype = pf_htons(ETH_TYPE_IP4);
    ah->hlen = 6;
    ah->plen = 4;
    ah->op = pf_htons(ARP_OP_REQUEST);
    memcpy(ah->sha, dev->mac, 6);
    pf_put_be32(ah->spa, dev->ip);
    memset(ah->tha, 0, 6); /* RFC 826: tha ignored in requests */
    pf_put_be32(ah->tpa, target_ip);

    stack->stats.arp_requests_tx++;
    eth_output(stack, dev, ETH_BCAST, ETH_TYPE_ARP, p);
}

static void arp_send_reply(struct pf_stack *stack, struct netdev *dev, const struct arp_hdr *req)
{
    struct pkt *p = pkt_alloc();
    pkt_reserve(p, PKT_TX_HEADROOM);
    struct arp_hdr *ah = pkt_put(p, ARP_HDR_LEN);

    /* RFC 826 reply algorithm: swap sender/target, insert our binding. */
    ah->htype = pf_htons(1);
    ah->ptype = pf_htons(ETH_TYPE_IP4);
    ah->hlen = 6;
    ah->plen = 4;
    ah->op = pf_htons(ARP_OP_REPLY);
    memcpy(ah->sha, dev->mac, 6);
    pf_put_be32(ah->spa, dev->ip);
    memcpy(ah->tha, req->sha, 6);
    memcpy(ah->tpa, req->spa, 4);

    stack->stats.arp_replies_tx++;
    eth_output(stack, dev, req->sha, ETH_TYPE_ARP, p);
}

void arp_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p)
{
    stack->stats.arp_rx++;

    if (p->len < ARP_HDR_LEN) {
        stack->stats.arp_rx_malformed++;
        goto out;
    }
    const struct arp_hdr *ah = (const struct arp_hdr *)p->data;
    if (pf_ntohs(ah->htype) != 1 || pf_ntohs(ah->ptype) != ETH_TYPE_IP4 || ah->hlen != 6 ||
        ah->plen != 4) {
        stack->stats.arp_rx_malformed++;
        goto out;
    }
    uint16_t op = pf_ntohs(ah->op);
    if (op != ARP_OP_REQUEST && op != ARP_OP_REPLY) {
        stack->stats.arp_rx_malformed++;
        goto out;
    }

    uint32_t spa = pf_get_be32(ah->spa);
    uint32_t tpa = pf_get_be32(ah->tpa);
    uint64_t now = pf_now_ms();
    bool for_us = dev->ip != 0 && tpa == dev->ip;

    /* RFC 826 merge: update an existing mapping from any ARP packet, but
     * only *create* one when we are the target — otherwise any chatty
     * neighbor could fill the cache (pollution defense). spa==0 is a DAD
     * probe (RFC 5227) and never learnable. */
    bool merged = false;
    if (spa != 0) {
        struct arp_entry *e = arp_lookup(&stack->arp, spa);
        if (e) {
            arp_entry_resolve(stack, e, dev, ah->sha, now);
            merged = true;
        } else if (for_us) {
            e = arp_alloc_entry(stack);
            e->ip = spa;
            arp_entry_resolve(stack, e, dev, ah->sha, now);
            merged = true;
        }
    }
    (void)merged;

    if (for_us && op == ARP_OP_REQUEST) {
        stack->stats.arp_req_for_us++;
        arp_send_reply(stack, dev, ah);
    } else if (op == ARP_OP_REPLY) {
        stack->stats.arp_replies_rx++;
    }

out:
    pkt_free(p);
}

int arp_resolve(struct pf_stack *stack, struct netdev *dev, uint32_t next_hop, uint8_t mac_out[6],
                struct pkt *p)
{
    struct arp_entry *e = arp_lookup(&stack->arp, next_hop);
    if (e && e->state == ARP_RESOLVED) {
        memcpy(mac_out, e->mac, 6);
        return 0;
    }

    if (!e) {
        e = arp_alloc_entry(stack);
        e->state = ARP_PENDING;
        e->ip = next_hop;
        e->dev = dev;
        e->tries = 1;
        e->deadline_ms = pf_now_ms() + ARP_PENDING_RETRY_MS;
        arp_send_request(stack, dev, next_hop);
    }

    if (p) {
        if (e->waitq.n >= ARP_WAITQ_MAX) {
            /* Bounded queue: drop the *oldest* so a burst keeps its newest
             * packets (TCP retransmits make this loss recoverable). */
            struct pkt *old = pktq_pop(&e->waitq);
            pkt_free(old);
            stack->stats.arp_waitq_drops++;
        }
        pktq_push(&e->waitq, p);
        stack->stats.arp_pkts_queued++;
    }
    return 1;
}

void arp_tick(struct pf_stack *stack, uint64_t now_ms)
{
    for (size_t i = 0; i < ARP_CACHE_SIZE; i++) {
        struct arp_entry *e = &stack->arp.entries[i];
        if (e->state == ARP_FREE || now_ms < e->deadline_ms)
            continue;

        if (e->state == ARP_RESOLVED) {
            /* RFC 1122 §2.3.2.1 — expire stale entries. */
            e->state = ARP_FREE;
            stack->stats.arp_cache_expired++;
        } else { /* ARP_PENDING */
            if (e->tries >= ARP_PENDING_MAX_TRIES) {
                stack->stats.arp_resolve_fails++;
                stack->stats.arp_waitq_drops += e->waitq.n;
                pktq_free_all(&e->waitq);
                e->state = ARP_FREE;
            } else {
                e->tries++;
                e->deadline_ms = now_ms + ARP_PENDING_RETRY_MS;
                arp_send_request(stack, e->dev, e->ip);
            }
        }
    }
}
