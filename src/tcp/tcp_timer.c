/* TCP timers: retransmission (RFC 6298 §5), delayed ACK, TIME_WAIT,
 * zero-window probing. Driven from the stack tick. */
#include "core/stack.h"
#include "tcp/tcp.h"

void tcp_tick(struct pf_stack *stack, uint64_t now_ms)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcp_cb *t = &stack->tcp.tcbs[i];
        if (!t->active)
            continue;

        if (t->state == TCP_TIME_WAIT) {
            if (now_ms >= t->timewait_deadline)
                tcb_free(stack, t); /* 2MSL elapsed */
            continue;
        }

        if (t->rto_deadline != 0 && now_ms >= t->rto_deadline) {
            t->rtx_count++;
            stack->stats.tcp_rto_fires++;

            if (t->rtx_count > TCP_MAX_RTX) {
                /* RFC 1122 §4.2.3.5 R2: give up and abort. */
                stack->stats.tcp_conn_timeouts++;
                tcp_send_ctl(stack, t, TCP_RST | TCP_ACK);
                tcp_conn_error(stack, t, PF_ETIMEDOUT);
                continue;
            }

            /* RFC 5681 §3.1 equation (4): timeout collapses the window. */
            uint32_t in_flight = t->snd_nxt - t->snd_una;
            if (in_flight > 0) {
                t->ssthresh = PF_MAX(in_flight / 2, 2u * t->mss);
                t->cwnd = t->mss;
                t->dupacks = 0;
            }

            /* RFC 6298 §5.5–5.7: back off, retransmit, restart. */
            t->rto_ms = PF_MIN(t->rto_ms * 2, (uint32_t)TCP_RTO_MAX_MS);
            tcp_retransmit(stack, t);
            t->rto_deadline = now_ms + t->rto_ms;
            continue;
        }

        /* Zero-window probe (RFC 9293 §3.8.6.1): data waiting, nothing in
         * flight, peer advertises zero — probe with one byte so the window
         * re-opening can't be lost with a dropped window update. */
        if (t->rto_deadline == 0 && t->snd_len > 0 && t->snd_wnd == 0 && t->snd_una == t->snd_nxt &&
            (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT ||
             t->state == TCP_FIN_WAIT_1 || t->state == TCP_LAST_ACK)) {
            stack->stats.tcp_zero_wnd_probes++;
            tcp_send_probe(stack, t);
            tcp_arm_rto(t, now_ms);
        }

        if (t->delack_deadline != 0 && now_ms >= t->delack_deadline)
            tcp_send_ctl(stack, t, TCP_ACK); /* resets delack state itself */
    }
}
