/*
 * Stack-wide protocol counters, declared once via X-macro so the struct,
 * the name table, and the CLI dump can never drift apart. Every drop has a
 * dedicated reason counter — "count, don't crash" is the contract for
 * malformed input.
 */
#ifndef PF_CORE_STATS_H
#define PF_CORE_STATS_H

#include "core/pf.h"

#define PF_STATS_MAP(X)                                                                            \
    X(eth_rx_frames)                                                                               \
    X(eth_rx_runts)                                                                                \
    X(eth_rx_unknown_ethertype)                                                                    \
    X(eth_rx_other_dest)                                                                           \
    X(eth_tx_frames)                                                                               \
    X(arp_rx)                                                                                      \
    X(arp_rx_malformed)                                                                            \
    X(arp_req_for_us)                                                                              \
    X(arp_replies_rx)                                                                              \
    X(arp_replies_tx)                                                                              \
    X(arp_requests_tx)                                                                             \
    X(arp_pkts_queued)                                                                             \
    X(arp_waitq_drops)                                                                             \
    X(arp_resolve_fails)                                                                           \
    X(arp_cache_expired)                                                                           \
    X(arp_cache_evictions)                                                                         \
    X(ip_rx)                                                                                       \
    X(ip_rx_truncated)                                                                             \
    X(ip_rx_bad_version)                                                                           \
    X(ip_rx_bad_ihl)                                                                               \
    X(ip_rx_bad_len)                                                                               \
    X(ip_rx_bad_csum)                                                                              \
    X(ip_rx_bad_src)                                                                               \
    X(ip_rx_bcast_ignored)                                                                         \
    X(ip_rx_not_for_us)                                                                            \
    X(ip_rx_delivered)                                                                             \
    X(ip_rx_proto_unreach)                                                                         \
    X(ip_frags_rx)                                                                                 \
    X(ip_reass_completed)                                                                          \
    X(ip_reass_timeouts)                                                                           \
    X(ip_reass_overlap_drops)                                                                      \
    X(ip_reass_too_big)                                                                            \
    X(ip_reass_bad_frag)                                                                           \
    X(ip_reass_evicted)                                                                            \
    X(ip_tx)                                                                                       \
    X(ip_tx_frags)                                                                                 \
    X(ip_tx_no_route)                                                                              \
    X(ip_fwd_forwarded)                                                                            \
    X(ip_fwd_ttl_exceeded)                                                                         \
    X(ip_fwd_no_route)                                                                             \
    X(ip_fwd_bad_dst)                                                                              \
    X(ip_fwd_mtu_drop)                                                                             \
    X(ip_fwd_same_if)                                                                              \
    X(icmp_rx)                                                                                     \
    X(icmp_rx_malformed)                                                                           \
    X(icmp_rx_bad_csum)                                                                            \
    X(icmp_echo_req_rx)                                                                            \
    X(icmp_echo_reply_tx)                                                                          \
    X(icmp_echo_reply_rx)                                                                          \
    X(icmp_rx_errors)                                                                              \
    X(icmp_rx_other)                                                                               \
    X(icmp_err_tx)                                                                                 \
    X(icmp_err_suppressed)                                                                         \
    X(udp_rx)                                                                                      \
    X(udp_rx_malformed)                                                                            \
    X(udp_rx_bad_csum)                                                                             \
    X(udp_rx_nocsum)                                                                               \
    X(udp_rx_no_sock)                                                                              \
    X(udp_rx_delivered)                                                                            \
    X(udp_rx_q_drops)                                                                              \
    X(udp_tx)                                                                                      \
    X(tcp_rx_segs)                                                                                 \
    X(tcp_rx_malformed)                                                                            \
    X(tcp_rx_bad_csum)                                                                             \
    X(tcp_rx_bad_opts)                                                                             \
    X(tcp_rx_no_match)                                                                             \
    X(tcp_rx_out_of_window)                                                                        \
    X(tcp_rx_bad_ack)                                                                              \
    X(tcp_rx_data_ignored)                                                                         \
    X(tcp_rx_bytes)                                                                                \
    X(tcp_tx_segs)                                                                                 \
    X(tcp_tx_bytes)                                                                                \
    X(tcp_rst_rx)                                                                                  \
    X(tcp_rst_tx)                                                                                  \
    X(tcp_challenge_acks_tx)                                                                       \
    X(tcp_rtx_segs)                                                                                \
    X(tcp_rto_fires)                                                                               \
    X(tcp_fast_rtx)                                                                                \
    X(tcp_dupacks_rx)                                                                              \
    X(tcp_ooo_queued)                                                                              \
    X(tcp_ooo_dropped)                                                                             \
    X(tcp_passive_opens)                                                                           \
    X(tcp_active_opens)                                                                            \
    X(tcp_conns_established)                                                                       \
    X(tcp_conns_reset)                                                                             \
    X(tcp_conn_timeouts)                                                                           \
    X(tcp_accept_drops)                                                                            \
    X(tcp_zero_wnd_probes)                                                                         \
    X(tcp_rtt_samples)

struct pf_stats {
#define X(name) uint64_t name;
    PF_STATS_MAP(X)
#undef X
};

size_t pf_stats_count(void);
const char *pf_stats_name(size_t i);
uint64_t pf_stats_get(const struct pf_stats *st, size_t i);

#endif /* PF_CORE_STATS_H */
