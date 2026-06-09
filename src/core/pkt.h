/*
 * Packet buffers: fixed-size, single-owner, malloc-backed (DECISIONS.md D-003).
 *
 * Layout:   buf[0 .. PKT_BUF_SIZE)
 *           ^----headroom----^ data[0 .. len) ^----tailroom----^
 *
 * RX frames are read at offset PKT_RX_OFFSET (=2) so the IPv4 header behind a
 * 14-byte Ethernet header lands 4-byte aligned (DECISIONS.md D-008).
 * TX packets reserve PKT_TX_HEADROOM, fill payload, then pf_push() headers.
 */
#ifndef PF_CORE_PKT_H
#define PF_CORE_PKT_H

#include "core/pf.h"

#define PKT_BUF_SIZE    4096 /* fits a reassembled datagram (DECISIONS.md D-010) */
#define PKT_RX_OFFSET   2
#define PKT_TX_HEADROOM 128 /* eth14 + ip(20+opts) + tcp(20+opts) with slack */

struct netdev;

struct pkt {
    struct pkt *next; /* intrusive singly-linked list */
    struct netdev *dev;
    uint64_t ts_ms; /* rx timestamp / last tx time (retransmit queue) */
    uint32_t u32;   /* scratch for owners (e.g. seg seq) */
    uint8_t *data;  /* current layer start, points into buf[] */
    uint16_t len;   /* bytes valid at data */
    _Alignas(4) uint8_t buf[PKT_BUF_SIZE];
};

struct pkt *pkt_alloc(void);
void pkt_free(struct pkt *p);

/* Set headroom before filling payload; only valid while len == 0. */
void pkt_reserve(struct pkt *p, uint16_t headroom);

/* Prepend n bytes (headers); returns new data pointer. */
void *pkt_push(struct pkt *p, uint16_t n);

/* Strip n bytes from the front; returns new data pointer. */
void *pkt_pull(struct pkt *p, uint16_t n);

/* Append n bytes at the tail; returns pointer to the appended region. */
void *pkt_put(struct pkt *p, uint16_t n);

/* Shrink to newlen bytes (drop tail). */
void pkt_trim(struct pkt *p, uint16_t newlen);

static inline uint16_t pkt_headroom(const struct pkt *p)
{
    return (uint16_t)(p->data - p->buf);
}
static inline uint16_t pkt_tailroom(const struct pkt *p)
{
    return (uint16_t)(PKT_BUF_SIZE - pkt_headroom(p) - p->len);
}

/* FIFO queue of packets. */
struct pktq {
    struct pkt *head, *tail;
    uint32_t n;
};

void pktq_init(struct pktq *q);
void pktq_push(struct pktq *q, struct pkt *p);
struct pkt *pktq_pop(struct pktq *q);
void pktq_free_all(struct pktq *q);
static inline bool pktq_empty(const struct pktq *q)
{
    return q->head == NULL;
}

#endif /* PF_CORE_PKT_H */
