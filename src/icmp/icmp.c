#include "icmp/icmp.h"
#include "core/stack.h"
#include "ipv4/checksum.h"
#include "netdev/netdev.h"

#include <string.h>

void icmp_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p,
                const struct ipv4_hdr *ih)
{
    (void)dev;
    stack->stats.icmp_rx++;

    uint8_t ihl = ip_hdr_len(ih);
    const uint8_t *icmp = p->data + ihl;
    uint16_t ilen = (uint16_t)(p->len - ihl);

    if (ilen < ICMP_HDR_LEN) {
        stack->stats.icmp_rx_malformed++;
        goto out;
    }
    /* RFC 792: checksum covers the whole ICMP message. */
    if (inet_csum(icmp, ilen) != 0) {
        stack->stats.icmp_rx_bad_csum++;
        goto out;
    }

    const struct icmp_hdr *ch = (const struct icmp_hdr *)icmp;
    switch (ch->type) {
    case ICMP_TYPE_ECHO_REQUEST:
        if (ch->code != 0) {
            stack->stats.icmp_rx_malformed++;
            break;
        }
        stack->stats.icmp_echo_req_rx++;
        {
            /* RFC 792 (Echo): return id, seq and data unchanged with type 0.
             * RFC 1122 §3.2.2.6: reply source is the request's destination. */
            struct pkt *rp = pkt_alloc();
            pkt_reserve(rp, PKT_TX_HEADROOM);
            struct icmp_hdr *rh = pkt_put(rp, ilen);
            memcpy(rh, icmp, ilen);
            rh->type = ICMP_TYPE_ECHO_REPLY;
            rh->csum = 0;
            rh->csum = pf_htons(inet_csum(rh, ilen));
            stack->stats.icmp_echo_reply_tx++;
            ip_output(stack, pf_ntohl(ih->daddr), pf_ntohl(ih->saddr), IP_PROTO_ICMP,
                      IP_DEFAULT_TTL, rp);
        }
        break;
    case ICMP_TYPE_ECHO_REPLY:
        stack->stats.icmp_echo_reply_rx++;
        break;
    case ICMP_TYPE_DEST_UNREACH:
    case ICMP_TYPE_SRC_QUENCH:
    case ICMP_TYPE_REDIRECT:
    case ICMP_TYPE_TIME_EXCEEDED:
    case ICMP_TYPE_PARAM_PROBLEM:
        /* Delivered to interested transports in later phases (UDP maps
         * port-unreachable onto sockets, etc.). Counted for now. */
        stack->stats.icmp_rx_errors++;
        break;
    default:
        /* RFC 1122 §3.2.3: silently discard unknown ICMP types. */
        stack->stats.icmp_rx_other++;
        break;
    }

out:
    pkt_free(p);
}

static bool icmp_type_is_error(uint8_t type)
{
    return type == ICMP_TYPE_DEST_UNREACH || type == ICMP_TYPE_SRC_QUENCH ||
           type == ICMP_TYPE_REDIRECT || type == ICMP_TYPE_TIME_EXCEEDED ||
           type == ICMP_TYPE_PARAM_PROBLEM;
}

void icmp_send_error(struct pf_stack *stack, const struct ipv4_hdr *orig, uint16_t orig_avail,
                     uint8_t type, uint8_t code)
{
    if (orig_avail < IPV4_HDR_MIN)
        return;
    uint8_t ihl = ip_hdr_len(orig);
    if (ihl < IPV4_HDR_MIN || ihl > orig_avail)
        return;

    /* RFC 1122 §3.2.2 suppression rules: never generate an error about an
     * ICMP error, a non-initial fragment, or non-unicast traffic. */
    if (pf_ntohs(orig->frag_off) & IP_FRAG_OFFMASK)
        return;
    uint32_t osrc = pf_ntohl(orig->saddr);
    uint32_t odst = pf_ntohl(orig->daddr);
    if (osrc == 0 || ip4_is_loopback(osrc) || ip4_is_multicast(osrc) || ip4_is_experimental(osrc) ||
        ip4_is_limited_bcast(osrc))
        return;
    if (ip4_is_multicast(odst) || ip4_is_limited_bcast(odst) || ip4_is_experimental(odst))
        return;
    if (orig->proto == IP_PROTO_ICMP && orig_avail >= (uint16_t)(ihl + 1)) {
        const uint8_t *oicmp = (const uint8_t *)orig + ihl;
        if (icmp_type_is_error(oicmp[0]))
            return;
    }

    /* RFC 1812 §4.3.2.8 — rate-limit generated errors. */
    uint64_t now = pf_now_ms();
    if (stack->icmp_last_err_ms != 0 && now - stack->icmp_last_err_ms < ICMP_ERR_MIN_INTERVAL_MS) {
        stack->stats.icmp_err_suppressed++;
        return;
    }

    /* RFC 792: error payload = original IP header + first 8 data octets. */
    uint16_t include = PF_MIN(orig_avail, (uint16_t)(ihl + 8));

    struct pkt *p = pkt_alloc();
    pkt_reserve(p, PKT_TX_HEADROOM);
    struct icmp_hdr *ch = pkt_put(p, (uint16_t)(ICMP_HDR_LEN + include));
    ch->type = type;
    ch->code = code;
    ch->csum = 0;
    ch->rest = 0;
    memcpy((uint8_t *)ch + ICMP_HDR_LEN, orig, include);
    ch->csum = pf_htons(inet_csum(ch, (size_t)(ICMP_HDR_LEN + include)));

    stack->stats.icmp_err_tx++;
    stack->icmp_last_err_ms = now;
    ip_output(stack, 0, osrc, IP_PROTO_ICMP, IP_DEFAULT_TTL, p);
}
