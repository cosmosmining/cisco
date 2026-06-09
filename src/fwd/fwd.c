/*
 * Forwarding plane: validate → LPM → TTL decrement (incremental checksum,
 * RFC 1624) → ARP-resolve next hop → transmit. Per RFC 1812 requirements
 * for IPv4 routers.
 */
#include "fwd/fwd.h"
#include "arp/arp.h"
#include "core/stack.h"
#include "eth/eth.h"
#include "icmp/icmp.h"
#include "ipv4/checksum.h"
#include "netdev/netdev.h"
#include "route/fib.h"

void ip_forward(struct pf_stack *stack, struct netdev *indev, struct pkt *p,
                const struct ipv4_hdr *ih_const)
{
    (void)indev;
    struct ipv4_hdr *ih = (struct ipv4_hdr *)ih_const; /* we own the buffer */
    uint32_t dst = pf_ntohl(ih->daddr);

    /* Never forward loopback/multicast/class-E destinations, nor any
     * subnet-directed broadcast of our interfaces (RFC 2644). */
    if (ip4_is_loopback(dst) || ip4_is_multicast(dst) || ip4_is_experimental(dst)) {
        stack->stats.ip_fwd_bad_dst++;
        goto drop;
    }
    for (int i = 0; i < stack->ndevs; i++) {
        struct netdev *d = stack->devs[i];
        if (d->ip != 0 && dst == (d->ip | ~d->mask)) {
            stack->stats.ip_fwd_bad_dst++;
            goto drop;
        }
    }

    /* RFC 1812 §5.3.1: TTL ≤ 1 → time-exceeded, do not forward. */
    if (ih->ttl <= 1) {
        stack->stats.ip_fwd_ttl_exceeded++;
        icmp_send_error(stack, ih, p->len, ICMP_TYPE_TIME_EXCEEDED, ICMP_TIME_EXC_TTL);
        goto drop;
    }

    struct fib_entry *rt = fib_lookup(&stack->fib, dst);
    if (!rt) {
        stack->stats.ip_fwd_no_route++;
        /* RFC 1812 §4.3.3.1: net-unreachable to the source. */
        icmp_send_error(stack, ih, p->len, ICMP_TYPE_DEST_UNREACH, ICMP_UNREACH_NET);
        goto drop;
    }
    struct netdev *out = rt->dev;

    if (p->len > out->mtu) {
        stack->stats.ip_fwd_mtu_drop++;
        if (pf_ntohs(ih->frag_off) & IP_FRAG_DF) /* RFC 1191 path-MTU signal */
            icmp_send_error(stack, ih, p->len, ICMP_TYPE_DEST_UNREACH, ICMP_UNREACH_FRAG_NEEDED);
        goto drop; /* no forwarding-path fragmentation (DECISIONS.md D-017) */
    }

    if (out == indev)
        stack->stats.ip_fwd_same_if++; /* hairpin; a full router would also
                                          consider an ICMP redirect here */

    /* TTL decrement with RFC 1624 incremental checksum update over the
     * 16-bit word that holds {ttl, proto}. */
    uint16_t old_word = (uint16_t)(((uint16_t)ih->ttl << 8) | ih->proto);
    ih->ttl--;
    uint16_t new_word = (uint16_t)(((uint16_t)ih->ttl << 8) | ih->proto);
    ih->csum = pf_htons(csum_update16(pf_ntohs(ih->csum), old_word, new_word));

    uint32_t next_hop = rt->connected ? dst : rt->next_hop;
    uint8_t mac[6];
    stack->stats.ip_fwd_forwarded++;
    if (arp_resolve(stack, out, next_hop, mac, p) == 0)
        eth_output(stack, out, mac, ETH_TYPE_IP4, p);
    /* else: parked on the ARP waitq; flushed or dropped by arp_tick */
    return;

drop:
    pkt_free(p);
}
