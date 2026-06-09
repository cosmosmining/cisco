/*
 * TCP connection management and segment input processing, following the
 * RFC 9293 §3.10.7 "SEGMENT ARRIVES" event processing (the consolidation
 * of RFC 793 with the RFC 1122 corrections).
 */
#include "tcp/tcp.h"
#include "core/stack.h"
#include "ipv4/checksum.h"
#include "netdev/netdev.h"
#include "udp/sock.h"

#include <stdlib.h>
#include <string.h>

void tcp_init(struct tcp_globals *tg)
{
    memset(tg, 0, sizeof(*tg));
    tg->iss_next = 0x1f2e3d4c;
}

void tcp_fini(struct tcp_globals *tg)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_cb *t = &tg->tcbs[i];
        if (t->active) {
            free(t->sndbuf);
            free(t->rcvbuf);
            pktq_free_all(&t->ooo);
            memset(t, 0, sizeof(*t));
        }
    }
}

struct tcp_cb *tcb_alloc(struct pf_stack *stack)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_cb *t = &stack->tcp.tcbs[i];
        if (t->active)
            continue;
        memset(t, 0, sizeof(*t));
        t->active = true;
        t->state = TCP_CLOSED;
        t->stack = stack;
        t->sock_idx = -1;
        t->mss = TCP_DEFAULT_MSS;
        t->srtt_ms = -1;
        t->rttvar_ms = -1;
        t->rto_ms = TCP_RTO_INITIAL_MS;
        t->ssthresh = 0xffffffffu;
        t->cwnd = 4 * TCP_DEFAULT_MSS;
        t->sndbuf = malloc(TCP_SNDBUF);
        t->rcvbuf = malloc(TCP_RCVBUF);
        PF_ASSERT(t->sndbuf && t->rcvbuf);
        pktq_init(&t->ooo);
        return t;
    }
    return NULL;
}

/* Detach the owning socket, marking it with an error for the API. */
static void tcb_detach_sock(struct pf_stack *stack, struct tcp_cb *tcb, int err)
{
    if (tcb->sock_idx >= 0) {
        struct pf_sock *s = &stack->socks.socks[tcb->sock_idx];
        s->tcb = NULL;
        s->err = err;
        tcb->sock_idx = -1;
    }
}

void tcb_free(struct pf_stack *stack, struct tcp_cb *tcb)
{
    /* Take embryonic and not-yet-accepted children down with a listener. */
    if (tcb->state == TCP_LISTEN) {
        for (int i = 0; i < TCP_MAX_CONNS; i++) {
            struct tcp_cb *c = &stack->tcp.tcbs[i];
            if (c->active && c != tcb && c->listener == tcb) {
                tcp_send_ctl(stack, c, TCP_RST | TCP_ACK);
                tcb_free(stack, c);
            }
        }
    }
    tcb_detach_sock(stack, tcb, 0);
    free(tcb->sndbuf);
    free(tcb->rcvbuf);
    pktq_free_all(&tcb->ooo);
    memset(tcb, 0, sizeof(*tcb));
}

static uint32_t tcp_gen_iss(struct pf_stack *stack)
{
    /* Clock-driven ISS in the spirit of RFC 793 §3.3. Not the hashed ISS
     * of RFC 6528 — fine for a lab stack, noted in DECISIONS.md D-016. */
    stack->tcp.iss_next += 64000u + (uint32_t)(pf_now_ms() & 0x3ff);
    return stack->tcp.iss_next;
}

static void tcp_cwnd_init(struct tcp_cb *tcb)
{
    /* RFC 5681 §3.1 initial window. */
    tcb->cwnd = PF_MIN(4u * tcb->mss, PF_MAX(2u * tcb->mss, 4380u));
    tcb->ssthresh = 0xffffffffu;
}

static void tcp_enter_timewait(struct tcp_cb *tcb)
{
    tcb->state = TCP_TIME_WAIT;
    tcb->timewait_deadline = pf_now_ms() + 2u * (uint64_t)TCP_MSL_MS; /* RFC 9293 §3.10.7 */
    tcb->rto_deadline = 0;
    tcb->delack_deadline = 0;
}

/* RFC 6298 §2 — SRTT/RTTVAR/RTO update from one measurement. */
static void tcp_rtt_sample(struct pf_stack *stack, struct tcp_cb *tcb, uint32_t r_ms)
{
    stack->stats.tcp_rtt_samples++;
    if (tcb->srtt_ms < 0) {
        tcb->srtt_ms = r_ms; /* §2.2 first measurement */
        tcb->rttvar_ms = (int64_t)r_ms / 2;
    } else {
        int64_t err = tcb->srtt_ms - (int64_t)r_ms;
        if (err < 0)
            err = -err;
        tcb->rttvar_ms = (3 * tcb->rttvar_ms + err) / 4;       /* §2.3 beta=1/4 */
        tcb->srtt_ms = (7 * tcb->srtt_ms + (int64_t)r_ms) / 8; /* alpha=1/8 */
    }
    int64_t rto = tcb->srtt_ms + PF_MAX((int64_t)TCP_CLOCK_G_MS, 4 * tcb->rttvar_ms);
    if (rto < TCP_RTO_MIN_MS)
        rto = TCP_RTO_MIN_MS;
    if (rto > TCP_RTO_MAX_MS)
        rto = TCP_RTO_MAX_MS;
    tcb->rto_ms = (uint32_t)rto;
}

static struct tcp_cb *tcb_lookup(struct pf_stack *stack, uint32_t lip, uint16_t lport, uint32_t rip,
                                 uint16_t rport)
{
    struct tcp_cb *listener = NULL;
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_cb *t = &stack->tcp.tcbs[i];
        if (!t->active)
            continue;
        if (t->state == TCP_LISTEN) {
            if (t->local_port == lport && (t->local_ip == 0 || t->local_ip == lip))
                listener = t;
            continue;
        }
        if (t->local_ip == lip && t->local_port == lport && t->remote_ip == rip &&
            t->remote_port == rport)
            return t;
    }
    return listener;
}

struct tcp_cb *tcp_open_listen(struct pf_stack *stack, int sock_idx, uint32_t ip, uint16_t port,
                               uint16_t backlog)
{
    struct tcp_cb *t = tcb_alloc(stack);
    if (!t)
        return NULL;
    t->state = TCP_LISTEN;
    t->sock_idx = sock_idx;
    t->local_ip = ip;
    t->local_port = port;
    t->backlog = (uint16_t)PF_MIN(backlog ? backlog : 1, TCP_ACCEPT_QLEN);
    return t;
}

struct tcp_cb *tcp_open_active(struct pf_stack *stack, int sock_idx, uint32_t lip, uint16_t lport,
                               uint32_t rip, uint16_t rport)
{
    struct tcp_cb *t = tcb_alloc(stack);
    if (!t)
        return NULL;
    t->sock_idx = sock_idx;
    t->local_ip = lip;
    t->local_port = lport;
    t->remote_ip = rip;
    t->remote_port = rport;
    t->iss = tcp_gen_iss(stack);
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1; /* SYN occupies one sequence number */
    t->state = TCP_SYN_SENT;
    stack->stats.tcp_active_opens++;
    tcp_send_ctl(stack, t, TCP_SYN);
    tcp_arm_rto(t, pf_now_ms());
    return t;
}

void tcp_app_close(struct pf_stack *stack, struct tcp_cb *tcb)
{
    switch (tcb->state) {
    case TCP_LISTEN:
    case TCP_SYN_SENT:
        tcb_free(stack, tcb);
        return;
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
        tcb->state = TCP_FIN_WAIT_1;
        break;
    case TCP_CLOSE_WAIT:
        tcb->state = TCP_LAST_ACK;
        break;
    default:
        return; /* already closing */
    }
    tcb->fin_queued = true;
    tcp_output(stack, tcb); /* emits FIN once the ring drains */
}

void tcp_app_abort(struct pf_stack *stack, struct tcp_cb *tcb)
{
    if (tcb->state != TCP_LISTEN && tcb->state != TCP_SYN_SENT)
        tcp_send_ctl(stack, tcb, TCP_RST | TCP_ACK); /* RFC 9293 ABORT call */
    tcb_free(stack, tcb);
}

/* Peer reset / fatal error: flush and tell the app. */
void tcp_conn_error(struct pf_stack *stack, struct tcp_cb *tcb, int err)
{
    stack->stats.tcp_conns_reset++;
    tcb_detach_sock(stack, tcb, err);
    tcb_free(stack, tcb);
}

/* Parse options of a SYN; returns peer MSS (0 if absent/bad). */
static uint16_t tcp_parse_mss(struct pf_stack *stack, const struct tcp_hdr *th)
{
    const uint8_t *o = (const uint8_t *)th + TCP_HDR_MIN;
    int left = tcp_hdr_len(th) - TCP_HDR_MIN;
    uint16_t mss = 0;
    while (left > 0) {
        uint8_t kind = o[0];
        if (kind == TCP_OPT_END)
            break;
        if (kind == TCP_OPT_NOP) {
            o++;
            left--;
            continue;
        }
        if (left < 2 || o[1] < 2 || o[1] > left) {
            stack->stats.tcp_rx_bad_opts++;
            break; /* malformed option list: stop parsing, keep the segment */
        }
        if (kind == TCP_OPT_MSS && o[1] == 4)
            mss = pf_get_be16(o + 2);
        left -= o[1];
        o += o[1];
    }
    return mss;
}

static void tcp_apply_mss(struct tcp_cb *tcb, uint16_t peer_mss)
{
    /* RFC 9293 §3.7.1: effective send MSS = min(peer MSS, ours). */
    tcb->mss = peer_mss ? (uint16_t)PF_MIN(peer_mss, TCP_OUR_MSS) : TCP_DEFAULT_MSS;
}

/* Append in-window bytes to the receive ring; returns bytes consumed. */
static uint32_t rcv_ring_put(struct tcp_cb *tcb, const uint8_t *data, uint32_t len)
{
    uint32_t space = TCP_RCVBUF - tcb->rcv_len;
    len = PF_MIN(len, space);
    uint32_t w = (tcb->rcv_head + tcb->rcv_len) % TCP_RCVBUF;
    uint32_t first = PF_MIN(len, TCP_RCVBUF - w);
    memcpy(tcb->rcvbuf + w, data, first);
    memcpy(tcb->rcvbuf, data + first, len - first);
    tcb->rcv_len += len;
    return len;
}

/* Pull parked out-of-order segments that are now contiguous. */
static void tcp_ooo_drain(struct pf_stack *stack, struct tcp_cb *tcb)
{
    while (tcb->ooo.head) {
        struct pkt *q = tcb->ooo.head;
        uint32_t qseq = q->u32;
        if (seq_gt(qseq, tcb->rcv_nxt))
            break; /* still a hole */
        pktq_pop(&tcb->ooo);
        if (seq_lt(qseq + q->len, tcb->rcv_nxt) || qseq + q->len == tcb->rcv_nxt) {
            pkt_free(q); /* entirely duplicate */
            continue;
        }
        uint32_t skip = tcb->rcv_nxt - qseq;
        uint32_t want = q->len - skip;
        uint32_t take = rcv_ring_put(tcb, q->data + skip, want);
        tcb->rcv_nxt += take;
        stack->stats.tcp_rx_bytes += take;
        pkt_free(q);
        if (take < want)
            break; /* ring full */
    }
}

static void tcp_ooo_insert(struct pf_stack *stack, struct tcp_cb *tcb, uint32_t seq,
                           const uint8_t *data, uint32_t len)
{
    if (tcb->ooo.n >= TCP_OOO_MAX) {
        stack->stats.tcp_ooo_dropped++;
        return;
    }
    /* Reject overlaps with already-parked segments: the retransmission will
     * refill anything we decline, and arbitration is where reassembly bugs
     * historically live. */
    for (struct pkt *q = tcb->ooo.head; q; q = q->next) {
        if (seq_lt(seq, q->u32 + q->len) && seq_lt(q->u32, seq + len)) {
            stack->stats.tcp_ooo_dropped++;
            return;
        }
    }
    struct pkt *q = pkt_alloc();
    memcpy(pkt_put(q, (uint16_t)len), data, len);
    q->u32 = seq;

    /* Sorted insert. */
    struct pkt **pp = &tcb->ooo.head;
    while (*pp && seq_lt((*pp)->u32, seq))
        pp = &(*pp)->next;
    q->next = *pp;
    *pp = q;
    if (!q->next)
        tcb->ooo.tail = q;
    tcb->ooo.n++;
    stack->stats.tcp_ooo_queued++;
}

/* In-window payload processing (step 7 of RFC 9293 §3.10.7.4). */
static void tcp_rx_payload(struct pf_stack *stack, struct tcp_cb *tcb, uint32_t seq,
                           const uint8_t *data, uint32_t len, uint64_t now)
{
    /* Trim the head overlap with already-received data. */
    if (seq_lt(seq, tcb->rcv_nxt)) {
        uint32_t skip = tcb->rcv_nxt - seq;
        if (skip >= len) {
            tcp_send_ctl(stack, tcb, TCP_ACK); /* pure duplicate: re-ACK */
            return;
        }
        data += skip;
        len -= skip;
        seq = tcb->rcv_nxt;
    }
    /* Trim the tail to our window. */
    uint32_t wnd_end = tcb->rcv_nxt + tcp_rcv_window(tcb);
    if (seq_ge(seq, wnd_end))
        return;
    if (seq_gt(seq + len, wnd_end))
        len = wnd_end - seq;
    if (len == 0)
        return;

    if (seq == tcb->rcv_nxt) {
        uint32_t take = rcv_ring_put(tcb, data, len);
        tcb->rcv_nxt += take;
        stack->stats.tcp_rx_bytes += take;
        tcp_ooo_drain(stack, tcb);

        /* RFC 1122 §4.2.3.2: ACK at least every second full segment;
         * otherwise delay (bounded by TCP_DELACK_MS). */
        tcb->delack_pending++;
        if (tcb->delack_pending >= 2)
            tcp_send_ctl(stack, tcb, TCP_ACK);
        else if (tcb->delack_deadline == 0)
            tcb->delack_deadline = now + TCP_DELACK_MS;
    } else {
        /* Hole: park the segment, and duplicate-ACK immediately so the
         * sender's fast retransmit can kick in (RFC 5681 §3.2). */
        tcp_ooo_insert(stack, tcb, seq, data, len);
        tcp_send_ctl(stack, tcb, TCP_ACK);
    }
}

void tcp_input(struct pf_stack *stack, struct netdev *dev, struct pkt *p, const struct ipv4_hdr *ih)
{
    (void)dev;
    stack->stats.tcp_rx_segs++;

    uint8_t ihl = ip_hdr_len(ih);
    uint16_t l4len = (uint16_t)(p->len - ihl);
    if (l4len < TCP_HDR_MIN) {
        stack->stats.tcp_rx_malformed++;
        goto out;
    }
    const struct tcp_hdr *th = (const struct tcp_hdr *)(p->data + ihl);
    uint8_t hl = tcp_hdr_len(th);
    if (hl < TCP_HDR_MIN || hl > l4len) {
        stack->stats.tcp_rx_malformed++;
        goto out;
    }

    uint32_t lip = pf_ntohl(ih->daddr), rip = pf_ntohl(ih->saddr);
    uint32_t sum = csum_pseudo(rip, lip, IP_PROTO_TCP, l4len);
    if (csum_fold(csum_partial(th, l4len, sum)) != 0) {
        stack->stats.tcp_rx_bad_csum++;
        goto out;
    }

    uint32_t seq = pf_ntohl(th->seq);
    uint32_t ack = pf_ntohl(th->ack);
    uint32_t wnd = pf_ntohs(th->wnd);
    uint8_t flags = th->flags;
    uint32_t paylen = (uint32_t)(l4len - hl);
    const uint8_t *payload = (const uint8_t *)th + hl;
    uint32_t seg_seqlen = paylen + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);
    uint64_t now = pf_now_ms();

    struct tcp_cb *tcb = tcb_lookup(stack, lip, pf_ntohs(th->dport), rip, pf_ntohs(th->sport));

    /* CLOSED (no TCB) — RFC 9293 §3.10.7.1: answer with RST. */
    if (!tcb) {
        stack->stats.tcp_rx_no_match++;
        tcp_send_reset_for(stack, ih, th, paylen);
        goto out;
    }

    /* LISTEN — RFC 9293 §3.10.7.2. */
    if (tcb->state == TCP_LISTEN) {
        if (flags & TCP_RST)
            goto out;
        if (flags & TCP_ACK) {
            tcp_send_reset_for(stack, ih, th, paylen);
            goto out;
        }
        if (!(flags & TCP_SYN))
            goto out;

        if (tcb->acceptq_n >= tcb->backlog) {
            stack->stats.tcp_accept_drops++; /* let the peer retry */
            goto out;
        }
        struct tcp_cb *c = tcb_alloc(stack);
        if (!c) {
            stack->stats.tcp_accept_drops++;
            goto out;
        }
        stack->stats.tcp_passive_opens++;
        c->local_ip = lip;
        c->local_port = tcb->local_port;
        c->remote_ip = rip;
        c->remote_port = pf_ntohs(th->sport);
        c->irs = seq;
        c->rcv_nxt = seq + 1;
        c->iss = tcp_gen_iss(stack);
        c->snd_una = c->iss;
        c->snd_nxt = c->iss + 1;
        c->snd_wnd = wnd;
        c->snd_wl1 = seq;
        c->snd_wl2 = c->iss;
        c->listener = tcb;
        tcp_apply_mss(c, tcp_parse_mss(stack, th));
        c->state = TCP_SYN_RCVD;
        tcp_send_ctl(stack, c, TCP_SYN | TCP_ACK);
        tcp_arm_rto(c, now);
        goto out;
    }

    /* SYN-SENT — RFC 9293 §3.10.7.3. */
    if (tcb->state == TCP_SYN_SENT) {
        if (flags & TCP_ACK) {
            if (seq_le(ack, tcb->iss) || seq_gt(ack, tcb->snd_nxt)) {
                if (!(flags & TCP_RST))
                    tcp_send_reset_for(stack, ih, th, paylen);
                goto out;
            }
        }
        if (flags & TCP_RST) {
            stack->stats.tcp_rst_rx++;
            if (flags & TCP_ACK) /* acceptable ACK + RST = connection refused */
                tcp_conn_error(stack, tcb, PF_ECONNREFUSED);
            goto out;
        }
        if (flags & TCP_SYN) {
            tcb->irs = seq;
            tcb->rcv_nxt = seq + 1;
            tcp_apply_mss(tcb, tcp_parse_mss(stack, th));
            if (flags & TCP_ACK)
                tcb->snd_una = ack;
            if (seq_gt(tcb->snd_una, tcb->iss)) { /* our SYN is acknowledged */
                tcb->state = TCP_ESTABLISHED;
                stack->stats.tcp_conns_established++;
                tcb->snd_wnd = wnd;
                tcb->snd_wl1 = seq;
                tcb->snd_wl2 = ack;
                tcp_cwnd_init(tcb);
                tcb->rto_deadline = 0;
                tcb->rtx_count = 0;
                tcp_send_ctl(stack, tcb, TCP_ACK);
            } else { /* simultaneous open */
                tcb->state = TCP_SYN_RCVD;
                tcp_send_ctl(stack, tcb, TCP_SYN | TCP_ACK);
                tcp_arm_rto(tcb, now);
            }
        }
        goto out;
    }

    /* ---- States SYN-RCVD and above: RFC 9293 §3.10.7.4 ---- */

    /* First: sequence-number acceptability. */
    uint32_t rcvw = tcp_rcv_window(tcb);
    bool acceptable;
    if (seg_seqlen == 0) {
        acceptable = (rcvw == 0) ? seq == tcb->rcv_nxt
                                 : (seq_ge(seq, tcb->rcv_nxt) && seq_lt(seq, tcb->rcv_nxt + rcvw));
    } else {
        acceptable =
            (rcvw > 0) && ((seq_ge(seq, tcb->rcv_nxt) && seq_lt(seq, tcb->rcv_nxt + rcvw)) ||
                           (seq_ge(seq + seg_seqlen - 1, tcb->rcv_nxt) &&
                            seq_lt(seq + seg_seqlen - 1, tcb->rcv_nxt + rcvw)));
    }
    if (!acceptable) {
        stack->stats.tcp_rx_out_of_window++;
        if (!(flags & TCP_RST))
            tcp_send_ctl(stack, tcb, TCP_ACK);
        goto out;
    }

    /* Second: RST. Exact-match required; an in-window-but-inexact RST gets
     * a challenge ACK (RFC 5961 §3.2 blind-reset defense). */
    if (flags & TCP_RST) {
        stack->stats.tcp_rst_rx++;
        if (seq == tcb->rcv_nxt) {
            /* Embryonic (back to conceptual LISTEN) and already-closing
             * connections die quietly; established ones surface the error. */
            if ((tcb->state == TCP_SYN_RCVD && tcb->listener) || tcb->state == TCP_CLOSING ||
                tcb->state == TCP_LAST_ACK || tcb->state == TCP_TIME_WAIT) {
                tcb_free(stack, tcb);
            } else {
                tcp_conn_error(stack, tcb, PF_ECONNRESET);
            }
        } else {
            stack->stats.tcp_challenge_acks_tx++;
            tcp_send_ctl(stack, tcb, TCP_ACK);
        }
        goto out;
    }

    /* Fourth: SYN in window → challenge ACK (RFC 5961 §4 defense; covers
     * half-open peers re-connecting after a crash). */
    if (flags & TCP_SYN) {
        stack->stats.tcp_challenge_acks_tx++;
        tcp_send_ctl(stack, tcb, TCP_ACK);
        goto out;
    }

    /* Fifth: ACK processing. */
    if (!(flags & TCP_ACK))
        goto out;

    if (tcb->state == TCP_SYN_RCVD) {
        if (seq_lt(tcb->snd_una, ack) && seq_le(ack, tcb->snd_nxt)) {
            tcb->state = TCP_ESTABLISHED;
            stack->stats.tcp_conns_established++;
            tcb->snd_una = ack;
            tcb->snd_wnd = wnd;
            tcb->snd_wl1 = seq;
            tcb->snd_wl2 = ack;
            tcp_cwnd_init(tcb);
            tcb->rto_deadline = 0;
            tcb->rtx_count = 0;
            /* Hand the connection to the listener's accept queue. */
            struct tcp_cb *l = tcb->listener;
            if (l && l->acceptq_n < l->backlog) {
                l->acceptq[l->acceptq_n++] = tcb;
            } else if (l) {
                stack->stats.tcp_accept_drops++;
                tcp_app_abort(stack, tcb);
                goto out;
            }
        } else {
            tcp_send_reset_for(stack, ih, th, paylen); /* RST <SEQ=SEG.ACK> */
            goto out;
        }
    }

    if (seq_gt(ack, tcb->snd_nxt)) { /* ACK for data never sent */
        stack->stats.tcp_rx_bad_ack++;
        tcp_send_ctl(stack, tcb, TCP_ACK);
        goto out;
    }

    if (seq_gt(ack, tcb->snd_una)) {
        /* New data acknowledged. */
        uint32_t acked = ack - tcb->snd_una;
        bool fin_acked = tcb->fin_sent && ack == tcb->snd_nxt;
        uint32_t dbytes = acked - (fin_acked ? 1 : 0);

        tcb->snd_head = (tcb->snd_head + dbytes) % TCP_SNDBUF;
        tcb->snd_len -= dbytes;
        tcb->snd_una = ack;

        if (tcb->rtt_timing && seq_ge(ack, tcb->rtt_seq)) {
            tcp_rtt_sample(stack, tcb, (uint32_t)(now - tcb->rtt_start_ms));
            tcb->rtt_timing = false;
        }
        tcb->rtx_count = 0;

        /* RFC 5681: an ACK that ends fast recovery only deflates cwnd to
         * ssthresh (§3.2 step 5); normal growth resumes on the next ACK. */
        if (tcb->dupacks >= 3) {
            tcb->cwnd = tcb->ssthresh;
        } else if (tcb->cwnd < tcb->ssthresh) {
            tcb->cwnd += PF_MIN(dbytes, (uint32_t)tcb->mss); /* slow start §3.1 */
        } else {
            tcb->cwnd += PF_MAX(1u, (uint32_t)tcb->mss * tcb->mss / tcb->cwnd); /* CA */
        }
        tcb->dupacks = 0;

        /* RFC 6298 §5.3: restart RTO while data remains outstanding. */
        tcb->rto_deadline = (tcb->snd_una == tcb->snd_nxt) ? 0 : now + tcb->rto_ms;

        if (fin_acked) {
            if (tcb->state == TCP_FIN_WAIT_1)
                tcb->state = TCP_FIN_WAIT_2;
            else if (tcb->state == TCP_CLOSING)
                tcp_enter_timewait(tcb);
            else if (tcb->state == TCP_LAST_ACK) {
                tcb_free(stack, tcb);
                goto out;
            }
        }
        tcp_output(stack, tcb); /* freed window may unblock more data */
    } else if (ack == tcb->snd_una && paylen == 0 && seg_seqlen == 0 && wnd == tcb->snd_wnd &&
               seq_lt(tcb->snd_una, tcb->snd_nxt)) {
        /* Duplicate ACK (RFC 5681 §2 definition). */
        stack->stats.tcp_dupacks_rx++;
        tcb->dupacks++;
        if (tcb->dupacks == 3) {
            /* Fast retransmit + fast recovery (§3.2). */
            uint32_t in_flight = tcb->snd_nxt - tcb->snd_una;
            tcb->ssthresh = PF_MAX(in_flight / 2, 2u * tcb->mss);
            tcp_retransmit(stack, tcb);
            stack->stats.tcp_fast_rtx++;
            tcb->cwnd = tcb->ssthresh + 3u * tcb->mss;
        } else if (tcb->dupacks > 3) {
            tcb->cwnd += tcb->mss;
            tcp_output(stack, tcb);
        }
    }

    /* Window update (RFC 9293 §3.10.7.4, SND.WND maintenance). */
    if (seq_le(tcb->snd_una, ack) && seq_le(ack, tcb->snd_nxt)) {
        if (seq_lt(tcb->snd_wl1, seq) || (tcb->snd_wl1 == seq && seq_le(tcb->snd_wl2, ack))) {
            bool was_zero = tcb->snd_wnd == 0;
            tcb->snd_wnd = wnd;
            tcb->snd_wl1 = seq;
            tcb->snd_wl2 = ack;
            if (was_zero && wnd > 0)
                tcp_output(stack, tcb);
        }
    }

    /* Seventh: segment text. */
    if (paylen > 0) {
        if (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_FIN_WAIT_1 ||
            tcb->state == TCP_FIN_WAIT_2)
            tcp_rx_payload(stack, tcb, seq, payload, paylen, now);
        else
            stack->stats.tcp_rx_data_ignored++; /* past our FIN: ignore */
    }

    /* Eighth: FIN. */
    if (flags & TCP_FIN) {
        uint32_t fin_seq = seq + paylen;
        if (fin_seq == tcb->rcv_nxt &&
            (tcb->state == TCP_ESTABLISHED || tcb->state == TCP_FIN_WAIT_1 ||
             tcb->state == TCP_FIN_WAIT_2 || tcb->state == TCP_TIME_WAIT)) {
            if (tcb->state != TCP_TIME_WAIT)
                tcb->rcv_nxt = fin_seq + 1;
            tcb->rcv_eof = true;
            tcp_send_ctl(stack, tcb, TCP_ACK);
            switch (tcb->state) {
            case TCP_ESTABLISHED:
                tcb->state = TCP_CLOSE_WAIT;
                break;
            case TCP_FIN_WAIT_1:
                /* Our FIN not yet acked (else we'd be in FIN_WAIT_2). */
                tcb->state = TCP_CLOSING;
                break;
            case TCP_FIN_WAIT_2:
                tcp_enter_timewait(tcb);
                break;
            case TCP_TIME_WAIT: /* retransmitted FIN: restart 2MSL */
                tcb->timewait_deadline = now + 2u * (uint64_t)TCP_MSL_MS;
                break;
            default:
                break;
            }
        }
        /* Out-of-order FIN: ignored; the dup-ACK from the payload path
         * already told the peer where we are. */
    }

out:
    pkt_free(p);
}
