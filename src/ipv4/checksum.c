#include "ipv4/checksum.h"

uint32_t csum_partial(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)p[0] << 8; /* RFC 1071: pad odd byte with zero */
    return sum;
}

uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

uint32_t csum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t l4len)
{
    uint32_t sum = 0;
    sum += src >> 16;
    sum += src & 0xffffu;
    sum += dst >> 16;
    sum += dst & 0xffffu;
    sum += proto;
    sum += l4len;
    return sum;
}

uint16_t csum_update16(uint16_t old_csum, uint16_t old_field, uint16_t new_field)
{
    /* RFC 1624 eqn. 3: HC' = ~(~HC + ~m + m'), all one's-complement adds. */
    uint32_t sum = (uint32_t)(uint16_t)~old_csum + (uint32_t)(uint16_t)~old_field + new_field;
    while (sum >> 16)
        sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}
