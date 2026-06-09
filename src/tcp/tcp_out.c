/* TCP segment construction and the send engine. */
#include "core/stack.h"
#include "ipv4/checksum.h"
#include "netdev/netdev.h"
#include "tcp/tcp.h"

#include <string.h>

uint16_t tcp_rcv_window(const struct tcp_cb *tcb)
{
    uint32_t freeb = TCP_RCVBUF - tcb->rcv_len;
    return (uint16_t)PF_MIN(freeb, 0xffffu);
}

void tcp_arm_rto(struct tcp_cb *tcb, uint64_t now)
{
    if (tcb->rto_deadline == 0)
        tcb->rto_deadline = now + tcb->rto_ms;
}

static void ring_copy_out(const uint8_t *ring, uint32_t start, uint8_t *dst, uint32_t n)
{
    start %= TCP_SNDBUF;
    uint32_t first = PF_MIN(n, TCP_SNDBUF - start);
    memcpy(dst, ring + start, first);
    memcpy(dst + first, ring, n - first);
}

/* Build and transmit one segment. `ring_off` is the byte offset from
 * snd_head for `paylen` payload bytes (ignored when paylen == 0). */
static void tcp_send_segment(struct pf_stack *stack, struct tcp_cb *tcb, uint8_t flags,
                             uint32_t seq, uint32_t ring_off, uint16_t paylen)
{
    bool with_mss = (flags & TCP_SYN) != 0;
    uint8_t optlen = with_mss ? 4 : 0;

    struct pkt *p = pkt_alloc();
    pkt_reserve(p, PKT_TX_HEADROOM);
    struct tcp_hdr *th = pkt_put(p, (uint16_t)(TCP_HDR_MIN + optlen));
    th->sport = pf_htons(tcb->local_port);
    th->dport = pf_htons(tcb->remote_port);
    th->seq = pf_htonl(seq);
    th->ack = (flags & TCP_ACK) ? pf_htonl(tcb->rcv_nxt) : 0;
    th->doff = (uint8_t)(((TCP_HDR_MIN + optlen) / 4) << 4);
    th->flags = flags;
    uint16_t wnd = tcp_rcv_window(tcb);
    th->wnd = pf_htons(wnd);
    th->csum = 0;
    th->urg = 0;
    if (with_mss) {
        /* MSS option — RFC 9293 §3.7.1. */
        uint8_t *o = (uint8_t *)th + TCP_HDR_MIN;
        o[0] = TCP_OPT_MSS;
        o[1] = 4;
        pf_put_be16(o + 2, TCP_OUR_MSS);
    }
    if (paylen)
        ring_copy_out(tcb->sndbuf, tcb->snd_head + ring_off, pkt_put(p, paylen), paylen);

    uint16_t seglen = (uint16_t)(TCP_HDR_MIN + optlen + paylen);
    uint32_t sum = csum_pseudo(tcb->local_ip, tcb->remote_ip, IP_PROTO_TCP, seglen);
    th->csum = pf_htons(csum_fold(csum_partial(th, seglen, sum)));

    if (flags & TCP_ACK) {
        tcb->delack_pending = 0;
        tcb->delack_deadline = 0;
        tcb->rcv_wnd_advertised = wnd;
    }

    stack->stats.tcp_tx_segs++;
    stack->stats.tcp_tx_bytes += paylen;
    ip_output(stack, tcb->local_ip, tcb->remote_ip, IP_PROTO_TCP, IP_DEFAULT_TTL, p);
}

/* One byte beyond a zero window — RFC 9293 §3.8.6.1. */
void tcp_send_probe(struct pf_stack *stack, struct tcp_cb *tcb)
{
    tcp_send_segment(stack, tcb, TCP_ACK, tcb->snd_nxt, tcb->snd_nxt - tcb->snd_una, 1);
    tcb->snd_nxt += 1;
}

void tcp_send_ctl(struct pf_stack *stack, struct tcp_cb *tcb, uint8_t flags)
{
    uint32_t seq = (flags & TCP_SYN) ? tcb->iss : tcb->snd_nxt;
    if (flags & TCP_RST)
        stack->stats.tcp_rst_tx++;
    tcp_send_segment(stack, tcb, flags, seq, 0, 0);
}

/* RST for a segment that matches no connection — RFC 9293 §3.10.7.1. */
void tcp_send_reset_for(struct pf_stack *stack, const struct ipv4_hdr *ih, const struct tcp_hdr *th,
                        uint32_t seg_paylen)
{
    if (th->flags & TCP_RST)
        return; /* never reset a reset */

    struct pkt *p = pkt_alloc();
    pkt_reserve(p, PKT_TX_HEADROOM);
    struct tcp_hdr *r = pkt_put(p, TCP_HDR_MIN);
    memset(r, 0, TCP_HDR_MIN);
    r->sport = th->dport;
    r->dport = th->sport;
    r->doff = (TCP_HDR_MIN / 4) << 4;

    uint32_t seglen =
        seg_paylen + ((th->flags & TCP_SYN) ? 1 : 0) + ((th->flags & TCP_FIN) ? 1 : 0);
    if (th->flags & TCP_ACK) {
        r->seq = th->ack; /* SEQ=SEG.ACK, no ACK flag */
        r->flags = TCP_RST;
    } else {
        r->seq = 0; /* SEQ=0, ACK=SEG.SEQ+SEG.LEN */
        r->ack = pf_htonl(pf_ntohl(th->seq) + seglen);
        r->flags = TCP_RST | TCP_ACK;
    }

    uint32_t src = pf_ntohl(ih->daddr), dst = pf_ntohl(ih->saddr);
    uint32_t sum = csum_pseudo(src, dst, IP_PROTO_TCP, TCP_HDR_MIN);
    r->csum = pf_htons(csum_fold(csum_partial(r, TCP_HDR_MIN, sum)));

    stack->stats.tcp_rst_tx++;
    ip_output(stack, src, dst, IP_PROTO_TCP, IP_DEFAULT_TTL, p);
}

/* Bytes of payload currently in flight (excludes SYN/FIN sequence bits). */
static uint32_t tcp_data_in_flight(const struct tcp_cb *tcb)
{
    uint32_t fl = tcb->snd_nxt - tcb->snd_una;
    if (tcb->fin_sent && fl > 0)
        fl--;
    return fl;
}

void tcp_output(struct pf_stack *stack, struct tcp_cb *tcb)
{
    if (tcb->state != TCP_ESTABLISHED && tcb->state != TCP_CLOSE_WAIT &&
        tcb->state != TCP_FIN_WAIT_1 && tcb->state != TCP_LAST_ACK)
        return;

    uint64_t now = pf_now_ms();
    uint32_t wnd = PF_MIN(tcb->snd_wnd, tcb->cwnd);

    for (;;) {
        uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
        uint32_t sent_data = tcp_data_in_flight(tcb);
        uint32_t avail = tcb->snd_len - sent_data;

        if (avail == 0 || tcb->fin_sent)
            break;
        if (in_flight >= wnd)
            break;

        uint32_t room = wnd - in_flight;
        uint16_t chunk = (uint16_t)PF_MIN(PF_MIN(avail, (uint32_t)tcb->mss), room);
        if (chunk == 0)
            break;

        /* Nagle (RFC 896, RFC 1122 §4.2.3.4): with data outstanding, hold
         * sub-MSS segments until everything is acknowledged. */
        if (!tcb->nodelay && sent_data > 0 && chunk < tcb->mss && chunk == avail)
            break;

        uint8_t flags = TCP_ACK;
        if (chunk == avail)
            flags |= TCP_PSH; /* emptying the buffer — push to the app */

        tcp_send_segment(stack, tcb, flags, tcb->snd_nxt, sent_data, chunk);
        tcb->snd_nxt += chunk;

        if (!tcb->rtt_timing) { /* one RTT sample in flight (RFC 6298 §3) */
            tcb->rtt_timing = true;
            tcb->rtt_seq = tcb->snd_nxt;
            tcb->rtt_start_ms = now;
        }
        tcp_arm_rto(tcb, now);
    }

    /* FIN goes out once the ring is fully transmitted. */
    if (tcb->fin_queued && !tcb->fin_sent && tcb->snd_len == tcp_data_in_flight(tcb)) {
        tcp_send_segment(stack, tcb, TCP_FIN | TCP_ACK, tcb->snd_nxt, 0, 0);
        tcb->fin_sent = true;
        tcb->snd_nxt += 1; /* FIN occupies one sequence number */
        tcp_arm_rto(tcb, now);
    }
}

void tcp_retransmit(struct pf_stack *stack, struct tcp_cb *tcb)
{
    stack->stats.tcp_rtx_segs++;
    tcb->rtt_timing = false; /* Karn's algorithm: never time retransmits */

    if (tcb->state == TCP_SYN_SENT) {
        tcp_send_ctl(stack, tcb, TCP_SYN);
        return;
    }
    if (tcb->state == TCP_SYN_RCVD) {
        tcp_send_ctl(stack, tcb, TCP_SYN | TCP_ACK);
        return;
    }

    uint32_t sent_data = tcp_data_in_flight(tcb);
    if (sent_data > 0) {
        uint16_t chunk = (uint16_t)PF_MIN(sent_data, (uint32_t)tcb->mss);
        uint8_t flags = TCP_ACK | (chunk == tcb->snd_len ? TCP_PSH : 0);
        tcp_send_segment(stack, tcb, flags, tcb->snd_una, 0, chunk);
    } else if (tcb->fin_sent && seq_lt(tcb->snd_una, tcb->snd_nxt)) {
        tcp_send_segment(stack, tcb, TCP_FIN | TCP_ACK, tcb->snd_una, 0, 0);
    }
}
