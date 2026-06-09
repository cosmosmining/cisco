#include "ipv4/ipv4.h"
#include "arp/arp.h"
#include "core/stack.h"
#include "eth/eth.h"
#include "fwd/fwd.h"
#include "icmp/icmp.h"
#include "ipv4/checksum.h"
#include "ipv4/ip_reass.h"
#include "netdev/netdev.h"
#include "tcp/tcp.h"
#include "udp/udp.h"

#include <string.h>

struct netdev *ip_route_lookup(struct pf_stack *stack, uint32_t dst, uint32_t *next_hop)
{
    struct fib_entry *rt = fib_lookup(&stack->fib, dst);
    if (!rt)
        return NULL;
    *next_hop = rt->connected ? dst : rt->next_hop;
    return rt->dev;
}

/* Is dst one of OUR addresses on any interface? (Weak host model,
 * RFC 1122 §3.3.4.2 — required so a router answers pings to its far
 * interface address.) */
static bool stack_owns_ip(const struct pf_stack *stack, uint32_t dst)
{
    for (int i = 0; i < stack->ndevs; i++)
        if (stack->devs[i]->ip != 0 && stack->devs[i]->ip == dst)
            return true;
    return false;
}

static bool ip4_bad_src(uint32_t src)
{
    /* RFC 1122 §3.2.1.3 — a datagram's source must be a unicast address. */
    return ip4_is_multicast(src) || ip4_is_experimental(src) || ip4_is_limited_bcast(src) ||
           ip4_is_loopback(src);
}

void ip_local_deliver(struct pf_stack *stack, struct netdev *dev, struct pkt *p,
                      const struct ipv4_hdr *ih)
{
    stack->stats.ip_rx_delivered++;

    switch (ih->proto) {
    case IP_PROTO_ICMP:
        icmp_input(stack, dev, p, ih); /* consumes p */
        return;
    case IP_PROTO_UDP:
        udp_input(stack, dev, p, ih); /* consumes p */
        return;
    case IP_PROTO_TCP:
        tcp_input(stack, dev, p, ih); /* consumes p */
        return;
    default:
        /* RFC 1122 §3.2.2.1 — unknown transport: dest-unreachable code 2
         * (protocol unreachable). */
        stack->stats.ip_rx_proto_unreach++;
        icmp_send_error(stack, ih, p->len, ICMP_TYPE_DEST_UNREACH, ICMP_UNREACH_PROTO);
        pkt_free(p);
        return;
    }
}

void ip_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p)
{
    stack->stats.ip_rx++;

    /* RFC 1122 §3.2.1 header validation, cheapest checks first;
     * every rejection is counted, never crashed on. */
    if (p->len < IPV4_HDR_MIN) {
        stack->stats.ip_rx_truncated++;
        goto drop;
    }
    const struct ipv4_hdr *ih = (const struct ipv4_hdr *)p->data;
    if (ip_hdr_version(ih) != 4) {
        stack->stats.ip_rx_bad_version++;
        goto drop;
    }
    uint8_t ihl = ip_hdr_len(ih);
    if (ihl < IPV4_HDR_MIN) {
        stack->stats.ip_rx_bad_ihl++;
        goto drop;
    }
    if (ihl > p->len) {
        stack->stats.ip_rx_truncated++;
        goto drop;
    }
    uint16_t total = pf_ntohs(ih->total_len);
    if (total < ihl) {
        stack->stats.ip_rx_bad_len++;
        goto drop;
    }
    if (total > p->len) { /* claims more bytes than arrived */
        stack->stats.ip_rx_bad_len++;
        goto drop;
    }
    if (total < p->len)
        pkt_trim(p, total); /* strip Ethernet min-frame padding */

    /* RFC 1071 verify: sum over the header including the checksum field
     * must fold to zero. */
    if (inet_csum(ih, ihl) != 0) {
        stack->stats.ip_rx_bad_csum++;
        goto drop;
    }

    uint32_t src = pf_ntohl(ih->saddr);
    uint32_t dst = pf_ntohl(ih->daddr);
    if (ip4_bad_src(src)) {
        stack->stats.ip_rx_bad_src++;
        goto drop;
    }

    if (!stack_owns_ip(stack, dst)) {
        uint32_t subnet_bcast = dev->ip | ~dev->mask;
        if (ip4_is_limited_bcast(dst) || ip4_is_multicast(dst) ||
            (dev->ip != 0 && dst == subnet_bcast)) {
            /* No broadcast/multicast services yet; count and drop. */
            stack->stats.ip_rx_bcast_ignored++;
            goto drop;
        }
        if (stack->forwarding) {
            ip_forward(stack, dev, p, ih); /* consumes p */
            return;
        }
        stack->stats.ip_rx_not_for_us++;
        goto drop;
    }

    uint16_t frag = pf_ntohs(ih->frag_off);
    if (frag & (IP_FRAG_MF | IP_FRAG_OFFMASK)) {
        stack->stats.ip_frags_rx++;
        ip_reass_input(stack, dev, p, ih); /* consumes p */
        return;
    }

    ip_local_deliver(stack, dev, p, ih);
    return;

drop:
    dev->st.rx_drops++;
    pkt_free(p);
}

/* Transmit one ready-made IP datagram chunk: build header, resolve, send. */
static int ip_output_one(struct pf_stack *stack, struct netdev *dev, uint32_t next_hop,
                         uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl, uint16_t id,
                         uint16_t frag_field, struct pkt *p)
{
    struct ipv4_hdr *ih = pkt_push(p, IPV4_HDR_MIN);
    ih->vihl = 0x45;
    ih->tos = 0;
    ih->total_len = pf_htons(p->len);
    ih->id = pf_htons(id);
    ih->frag_off = pf_htons(frag_field);
    ih->ttl = ttl;
    ih->proto = proto;
    ih->csum = 0;
    ih->saddr = pf_htonl(src);
    ih->daddr = pf_htonl(dst);
    ih->csum = pf_htons(inet_csum(ih, IPV4_HDR_MIN));

    stack->stats.ip_tx++;

    uint32_t subnet_bcast = dev->ip | ~dev->mask;
    if (ip4_is_limited_bcast(dst) || (dev->ip != 0 && dst == subnet_bcast))
        return eth_output(stack, dev, ETH_BCAST, ETH_TYPE_IP4, p);

    uint8_t mac[6];
    if (arp_resolve(stack, dev, next_hop, mac, p) == 0)
        return eth_output(stack, dev, mac, ETH_TYPE_IP4, p);
    return 0; /* parked on the ARP waitq */
}

int ip_output(struct pf_stack *stack, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl,
              struct pkt *p)
{
    uint32_t next_hop;
    struct netdev *dev = ip_route_lookup(stack, dst, &next_hop);
    if (!dev || dst == dev->ip) { /* no self-delivery via the wire */
        stack->stats.ip_tx_no_route++;
        pkt_free(p);
        return -1;
    }
    if (src == 0)
        src = dev->ip;

    uint16_t id = stack->ip_id++;

    if (p->len + IPV4_HDR_MIN <= dev->mtu)
        return ip_output_one(stack, dev, next_hop, src, dst, proto, ttl, id, 0, p);

    /* RFC 791 §3.2 sender-side fragmentation: split the payload on 8-byte
     * boundaries; every fragment but the last carries MF. */
    uint16_t max_chunk = (uint16_t)((dev->mtu - IPV4_HDR_MIN) & ~7u);
    uint16_t off = 0;
    int rc = 0;
    while (off < p->len) {
        uint16_t chunk = PF_MIN(max_chunk, (uint16_t)(p->len - off));
        bool last = off + chunk == p->len;

        struct pkt *fp = pkt_alloc();
        pkt_reserve(fp, PKT_TX_HEADROOM);
        memcpy(pkt_put(fp, chunk), p->data + off, chunk);

        uint16_t frag_field = (uint16_t)((off / 8) & IP_FRAG_OFFMASK);
        if (!last)
            frag_field |= IP_FRAG_MF;

        stack->stats.ip_tx_frags++;
        if (ip_output_one(stack, dev, next_hop, src, dst, proto, ttl, id, frag_field, fp) < 0)
            rc = -1;
        off = (uint16_t)(off + chunk);
    }
    pkt_free(p);
    return rc;
}
