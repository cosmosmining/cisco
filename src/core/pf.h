/*
 * PacketForge core definitions shared by every module.
 *
 * Protocol modules (eth/arp/ipv4/icmp/udp/tcp/route/fwd) may include only
 * core headers and each other — never platform headers — so the datapath
 * stays portable to bare-metal targets (DECISIONS.md D-005).
 */
#ifndef PF_CORE_PF_H
#define PF_CORE_PF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- byte order -------------------------------------------------------- */

#if !defined(__BYTE_ORDER__) || !defined(__ORDER_LITTLE_ENDIAN__)
#error "compiler must define __BYTE_ORDER__"
#endif

static inline uint16_t pf_htons(uint16_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return (uint16_t)((uint16_t)(x << 8) | (uint16_t)(x >> 8));
#else
    return x;
#endif
}
static inline uint16_t pf_ntohs(uint16_t x)
{
    return pf_htons(x);
}

static inline uint32_t pf_htonl(uint32_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(x);
#else
    return x;
#endif
}
static inline uint32_t pf_ntohl(uint32_t x)
{
    return pf_htonl(x);
}

/* ---- time --------------------------------------------------------------
 * Monotonic milliseconds since an arbitrary epoch. The Linux platform layer
 * provides it (src/netdev/clock_linux.c); unit tests link their own
 * implementation to inject a fake clock. */
uint64_t pf_now_ms(void);

/* ---- logging ------------------------------------------------------------ */

enum pf_log_level { PF_LOG_ERR = 0, PF_LOG_WARN, PF_LOG_INFO, PF_LOG_DEBUG };

extern enum pf_log_level pf_log_threshold; /* global knob, see DECISIONS.md D-007 */

void pf_logf(enum pf_log_level lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define PF_ERR(...)   pf_logf(PF_LOG_ERR, __VA_ARGS__)
#define PF_WARN(...)  pf_logf(PF_LOG_WARN, __VA_ARGS__)
#define PF_INFO(...)  pf_logf(PF_LOG_INFO, __VA_ARGS__)
#define PF_DEBUG(...) pf_logf(PF_LOG_DEBUG, __VA_ARGS__)

void pf_hexdump(const void *buf, size_t len);

/* Fatal invariant violation: log and abort. Use for programming errors only,
 * never for malformed input off the wire (that gets dropped + counted). */
void pf_panic(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

#define PF_ASSERT(cond)                                                                            \
    do {                                                                                           \
        if (!(cond))                                                                               \
            pf_panic("assert failed: %s (%s:%d)", #cond, __FILE__, __LINE__);                      \
    } while (0)

/* ---- small helpers ------------------------------------------------------ */

#define PF_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define PF_MIN(a, b)    ((a) < (b) ? (a) : (b))
#define PF_MAX(a, b)    ((a) > (b) ? (a) : (b))

/* Dotted-quad formatting; `ip` in host byte order. Returns `buf`. */
const char *pf_ip4_str(uint32_t ip, char buf[16]);

/* Parse "a.b.c.d" into host-byte-order ip. Returns false on bad input. */
bool pf_ip4_parse(const char *s, uint32_t *out);

const char *pf_mac_str(const uint8_t mac[6], char buf[18]);

#endif /* PF_CORE_PF_H */
