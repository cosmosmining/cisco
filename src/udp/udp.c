#include "udp/udp.h"
#include "core/stack.h"
#include "icmp/icmp.h"
#include "ipv4/checksum.h"
#include "netdev/netdev.h"
#include "udp/sock.h"

#include <string.h>

void udp_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p, const struct ipv4_hdr *ih)
{
    (void)dev;
    stack->stats.udp_rx++;

    uint8_t ihl = ip_hdr_len(ih);
    uint16_t l4len = (uint16_t)(p->len - ihl);
    if (l4len < UDP_HDR_LEN) {
        stack->stats.udp_rx_malformed++;
        goto drop;
    }
    const struct udp_hdr *uh = (const struct udp_hdr *)(p->data + ihl);
    uint16_t ulen = pf_ntohs(uh->len);
    /* RFC 768: length covers header + data and can't exceed what IP
     * delivered. Shorter is tolerated (trailing junk ignored). */
    if (ulen < UDP_HDR_LEN || ulen > l4len) {
        stack->stats.udp_rx_malformed++;
        goto drop;
    }

    uint32_t src = pf_ntohl(ih->saddr);
    uint32_t dst = pf_ntohl(ih->daddr);

    if (uh->csum == 0) {
        /* RFC 768: an all-zero checksum means "not computed" (IPv4 only). */
        stack->stats.udp_rx_nocsum++;
    } else {
        /* Verify over pseudo-header + UDP header + data (RFC 768). */
        uint32_t sum = csum_pseudo(src, dst, IP_PROTO_UDP, ulen);
        if (csum_fold(csum_partial(uh, ulen, sum)) != 0) {
            stack->stats.udp_rx_bad_csum++;
            goto drop;
        }
    }

    struct pf_sock *sk = pf_sock_lookup_udp(stack, dst, pf_ntohs(uh->dport));
    if (!sk) {
        /* RFC 1122 §4.1.3.1: closed port → ICMP port unreachable. */
        stack->stats.udp_rx_no_sock++;
        icmp_send_error(stack, ih, p->len, ICMP_TYPE_DEST_UNREACH, ICMP_UNREACH_PORT);
        goto drop;
    }

    if (sk->rxq.n >= PF_SOCK_RXQ_CAP) {
        stack->stats.udp_rx_q_drops++;
        goto drop;
    }

    /* Queue with data at the UDP payload; remember the peer. */
    pkt_pull(p, (uint16_t)(ihl + UDP_HDR_LEN));
    pkt_trim(p, (uint16_t)(ulen - UDP_HDR_LEN));
    p->meta_ip = src;
    p->meta_port = pf_ntohs(uh->sport);
    pktq_push(&sk->rxq, p);
    stack->stats.udp_rx_delivered++;
    return;

drop:
    pkt_free(p);
}

int udp_output(struct pf_stack *stack, uint32_t src_ip, uint16_t sport, uint32_t dst_ip,
               uint16_t dport, struct pkt *p)
{
    uint16_t ulen = (uint16_t)(p->len + UDP_HDR_LEN);
    struct udp_hdr *uh = pkt_push(p, UDP_HDR_LEN);
    uh->sport = pf_htons(sport);
    uh->dport = pf_htons(dport);
    uh->len = pf_htons(ulen);
    uh->csum = 0;

    if (src_ip == 0) {
        /* The checksum needs the source address now; resolve the route the
         * same way ip_output will. */
        uint32_t nh;
        struct netdev *dev = ip_route_lookup(stack, dst_ip, &nh);
        if (!dev) {
            stack->stats.ip_tx_no_route++;
            pkt_free(p);
            return -1;
        }
        src_ip = dev->ip;
    }

    uint32_t sum = csum_pseudo(src_ip, dst_ip, IP_PROTO_UDP, ulen);
    uint16_t c = csum_fold(csum_partial(uh, ulen, sum));
    /* RFC 768: a computed checksum of zero is transmitted as all ones. */
    uh->csum = pf_htons(c == 0 ? 0xffff : c);

    stack->stats.udp_tx++;
    return ip_output(stack, src_ip, dst_ip, IP_PROTO_UDP, IP_DEFAULT_TTL, p);
}
