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
    X(arp_cache_evictions)

struct pf_stats {
#define X(name) uint64_t name;
    PF_STATS_MAP(X)
#undef X
};

size_t pf_stats_count(void);
const char *pf_stats_name(size_t i);
uint64_t pf_stats_get(const struct pf_stats *st, size_t i);

#endif /* PF_CORE_STATS_H */
