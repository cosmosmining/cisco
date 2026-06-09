#include "ipv4/ip_reass.h"
#include "core/stack.h"
#include "icmp/icmp.h"
#include "ipv4/checksum.h"
#include "netdev/netdev.h"

#include <stdlib.h>
#include <string.h>

void ip_reass_init(struct ip_reass *r)
{
    memset(r, 0, sizeof(*r));
}

static void ctx_release(struct ip_reass_ctx *c)
{
    free(c->payload);
    memset(c, 0, sizeof(*c));
}

void ip_reass_fini(struct ip_reass *r)
{
    for (int i = 0; i < IP_REASS_CTXS; i++)
        if (r->ctxs[i].in_use)
            ctx_release(&r->ctxs[i]);
}

static struct ip_reass_ctx *ctx_find(struct ip_reass *r, uint32_t src, uint32_t dst, uint16_t id,
                                     uint8_t proto)
{
    /* RFC 791 §3.2: reassembly key is (src, dst, id, protocol). */
    for (int i = 0; i < IP_REASS_CTXS; i++) {
        struct ip_reass_ctx *c = &r->ctxs[i];
        if (c->in_use && c->src == src && c->dst == dst && c->id == id && c->proto == proto)
            return c;
    }
    return NULL;
}

static struct ip_reass_ctx *ctx_alloc(struct pf_stack *stack)
{
    struct ip_reass *r = &stack->reass;
    struct ip_reass_ctx *victim = &r->ctxs[0];
    for (int i = 0; i < IP_REASS_CTXS; i++) {
        struct ip_reass_ctx *c = &r->ctxs[i];
        if (!c->in_use) {
            victim = c;
            goto take;
        }
        if (c->deadline_ms < victim->deadline_ms)
            victim = c;
    }
    /* Table full: evict the context closest to its timeout so floods can't
     * pin the table forever, and a fresh datagram can still reassemble. */
    stack->stats.ip_reass_evicted++;
    ctx_release(victim);
take:
    victim->in_use = true;
    victim->expected_total = -1;
    victim->payload = malloc(IP_REASS_MAX_PAYLOAD);
    PF_ASSERT(victim->payload != NULL);
    return victim;
}

/* Insert [start,end) rejecting ANY overlap with existing data: overlapping
 * fragments are only ever produced by broken or malicious senders
 * (teardrop-style attacks craft them to confuse reassembly into corrupting
 * memory or bypassing filters), so the whole context is aborted — the
 * stance RFC 5722 mandates for IPv6 and Linux adopted for IPv4 after
 * CVE-2018-5391. Returns false if the context must die. */
static bool ranges_insert(struct ip_reass_ctx *c, uint16_t start, uint16_t end)
{
    int pos = c->nranges;
    for (int i = 0; i < c->nranges; i++) {
        if (start < c->ranges[i].end && c->ranges[i].start < end)
            return false; /* overlap (duplicates included) */
        if (end <= c->ranges[i].start) {
            pos = i;
            break;
        }
    }
    if (c->nranges >= IP_REASS_MAX_RANGES)
        return false; /* absurdly fragmented — treat as hostile */

    memmove(&c->ranges[pos + 1], &c->ranges[pos],
            (size_t)(c->nranges - pos) * sizeof(c->ranges[0]));
    c->ranges[pos].start = start;
    c->ranges[pos].end = end;
    c->nranges++;

    /* Coalesce exact-adjacent neighbors. */
    for (int i = c->nranges - 2; i >= 0; i--) {
        if (c->ranges[i].end == c->ranges[i + 1].start) {
            c->ranges[i].end = c->ranges[i + 1].end;
            memmove(&c->ranges[i + 1], &c->ranges[i + 2],
                    (size_t)(c->nranges - i - 2) * sizeof(c->ranges[0]));
            c->nranges--;
        }
    }
    return true;
}

static void deliver_complete(struct pf_stack *stack, struct ip_reass_ctx *c)
{
    uint16_t total = (uint16_t)c->expected_total;

    struct pkt *p = pkt_alloc();
    struct ipv4_hdr *ih = pkt_put(p, c->hdr_len);
    memcpy(ih, c->hdr, c->hdr_len);
    memcpy(pkt_put(p, total), c->payload, total);

    /* Rebuilt datagram: first fragment's header with fragmentation cleared
     * and lengths/checksum corrected. */
    ih->total_len = pf_htons((uint16_t)(c->hdr_len + total));
    ih->frag_off = 0;
    ih->csum = 0;
    ih->csum = pf_htons(inet_csum(ih, c->hdr_len));

    struct netdev *dev = c->dev;
    stack->stats.ip_reass_completed++;
    ctx_release(c);
    ip_local_deliver(stack, dev, p, (const struct ipv4_hdr *)p->data);
}

void ip_reass_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p,
                    const struct ipv4_hdr *ih)
{
    uint16_t frag = pf_ntohs(ih->frag_off);
    uint16_t off = (uint16_t)((frag & IP_FRAG_OFFMASK) * 8);
    bool mf = (frag & IP_FRAG_MF) != 0;
    uint8_t ihl = ip_hdr_len(ih);
    uint16_t paylen = (uint16_t)(p->len - ihl);

    /* RFC 791 §3.2: every fragment except the last carries a multiple of
     * 8 payload bytes. Zero-length fragments are equally bogus. */
    if (paylen == 0 || (mf && (paylen & 7) != 0)) {
        stack->stats.ip_reass_bad_frag++;
        goto out;
    }

    uint32_t src = pf_ntohl(ih->saddr);
    uint32_t dst = pf_ntohl(ih->daddr);
    struct ip_reass_ctx *c = ctx_find(&stack->reass, src, dst, pf_ntohs(ih->id), ih->proto);
    if (!c) {
        c = ctx_alloc(stack);
        c->src = src;
        c->dst = dst;
        c->id = pf_ntohs(ih->id);
        c->proto = ih->proto;
        c->dev = dev;
        c->deadline_ms = pf_now_ms() + IP_REASS_TIMEOUT_MS;
    }

    if ((uint32_t)off + paylen > IP_REASS_MAX_PAYLOAD) {
        /* Bounded reassembly buffer (DECISIONS.md D-010). */
        stack->stats.ip_reass_too_big++;
        ctx_release(c);
        goto out;
    }

    if (!mf) {
        /* Last fragment pins the total. A second, conflicting "last" is
         * hostile. */
        int32_t end = off + paylen;
        if (c->expected_total >= 0 && c->expected_total != end) {
            stack->stats.ip_reass_overlap_drops++;
            ctx_release(c);
            goto out;
        }
        c->expected_total = end;
    }
    if (c->expected_total >= 0 && (int32_t)(off + paylen) > c->expected_total) {
        stack->stats.ip_reass_overlap_drops++; /* data past the declared end */
        ctx_release(c);
        goto out;
    }

    if (!ranges_insert(c, off, (uint16_t)(off + paylen))) {
        stack->stats.ip_reass_overlap_drops++;
        ctx_release(c);
        goto out;
    }
    memcpy(c->payload + off, p->data + ihl, paylen);

    if (off == 0) {
        c->have_first = true;
        memcpy(c->hdr, ih, ihl);
        c->hdr_len = ihl;
    }

    if (c->have_first && c->expected_total >= 0 && c->nranges == 1 && c->ranges[0].start == 0 &&
        c->ranges[0].end == (uint16_t)c->expected_total)
        deliver_complete(stack, c);

out:
    pkt_free(p);
}

void ip_reass_tick(struct pf_stack *stack, uint64_t now_ms)
{
    for (int i = 0; i < IP_REASS_CTXS; i++) {
        struct ip_reass_ctx *c = &stack->reass.ctxs[i];
        if (!c->in_use || now_ms < c->deadline_ms)
            continue;

        stack->stats.ip_reass_timeouts++;
        /* RFC 1122 §3.3.2: on reassembly timeout send time-exceeded code 1,
         * but only if fragment zero was received. */
        if (c->have_first) {
            uint8_t tmp[IPV4_HDR_MAX + 8];
            uint16_t data8 = 0;
            if (c->nranges > 0 && c->ranges[0].start == 0)
                data8 = PF_MIN((uint16_t)8, c->ranges[0].end);
            memcpy(tmp, c->hdr, c->hdr_len);
            memcpy(tmp + c->hdr_len, c->payload, data8);
            icmp_send_error(stack, (const struct ipv4_hdr *)tmp, (uint16_t)(c->hdr_len + data8),
                            ICMP_TYPE_TIME_EXCEEDED, ICMP_TIME_EXC_REASS);
        }
        ctx_release(c);
    }
}
