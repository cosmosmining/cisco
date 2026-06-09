/*
 * Internet checksum — RFC 1071, shared by IPv4/ICMP/UDP/TCP — plus the
 * incremental-update form of RFC 1624 used by the forwarding path.
 *
 * Convention: sums are accumulated over big-endian 16-bit words and all
 * checksum values handled here are plain numbers (host order); convert with
 * pf_htons exactly once when writing into a wire header.
 */
#ifndef PF_IPV4_CHECKSUM_H
#define PF_IPV4_CHECKSUM_H

#include "core/pf.h"

/* Accumulate `len` bytes into a running 32-bit sum (RFC 1071 §4.1; odd
 * trailing byte is padded with zero on the right). */
uint32_t csum_partial(const void *data, size_t len, uint32_t sum);

/* Fold carries and complement. */
uint16_t csum_fold(uint32_t sum);

static inline uint16_t inet_csum(const void *data, size_t len)
{
    return csum_fold(csum_partial(data, len, 0));
}

/* Pseudo-header sum for UDP/TCP (RFC 768 / RFC 9293 §3.1).
 * Addresses in host byte order; result feeds csum_partial's `sum`. */
uint32_t csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t l4len);

/* RFC 1624 eqn. 3: recompute a checksum after changing one 16-bit field.
 * All three arguments are numbers (host order). */
uint16_t csum_update16(uint16_t old_csum, uint16_t old_field, uint16_t new_field);

#endif /* PF_IPV4_CHECKSUM_H */
