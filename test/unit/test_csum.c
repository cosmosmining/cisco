#include "ut.h"

#include "ipv4/checksum.h"

#include <string.h>

static void test_rfc1071_example(void)
{
    /* RFC 1071 §3 worked example: words 0001 f203 f4f5 f6f7. */
    const uint8_t data[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    UT_ASSERT_EQ(inet_csum(data, sizeof(data)), 0x220d);
}

static void test_odd_length_pads_right(void)
{
    /* RFC 1071: odd byte is padded with zero on the right → word 0x0100. */
    const uint8_t one[] = {0x01};
    UT_ASSERT_EQ(inet_csum(one, 1), (uint16_t)~0x0100);

    const uint8_t three[] = {0x12, 0x34, 0x56};
    UT_ASSERT_EQ(inet_csum(three, 3), (uint16_t) ~(0x1234 + 0x5600));
}

static void test_carry_folding(void)
{
    /* ffff+ffff+ffff+0102 = 0x300ff → fold 0x00ff+3 = 0x0102 → ~ = 0xfefd. */
    const uint8_t data[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x02};
    UT_ASSERT_EQ(inet_csum(data, sizeof(data)), 0xfefd);

    /* All-ones identity: sums to 0xffff after folding, checksum 0. */
    const uint8_t ones[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    UT_ASSERT_EQ(inet_csum(ones, sizeof(ones)), 0x0000);
}

static void test_verify_includes_checksum_field(void)
{
    uint8_t hdr[20];
    for (size_t i = 0; i < sizeof(hdr); i++)
        hdr[i] = (uint8_t)(i * 7 + 1);
    hdr[10] = hdr[11] = 0; /* checksum field */
    uint16_t c = inet_csum(hdr, sizeof(hdr));
    hdr[10] = (uint8_t)(c >> 8);
    hdr[11] = (uint8_t)c;
    UT_ASSERT_EQ(inet_csum(hdr, sizeof(hdr)), 0);
}

static void test_incremental_update_matches_recompute(void)
{
    /* RFC 1624 eqn. 3 must equal a full recompute when one 16-bit field
     * changes — exercised exactly like a router's TTL decrement. */
    uint8_t hdr[20] = {0x45, 0x00, 0x00, 0x54, 0xbe, 0xef, 0x40, 0x00, 0x40, 0x01,
                       0x00, 0x00, 0x0a, 0xbe, 0x00, 0x01, 0x0a, 0xbe, 0x00, 0x02};
    uint16_t c = inet_csum(hdr, sizeof(hdr));

    for (uint8_t ttl_new = 0; ttl_new < 0x41; ttl_new++) {
        uint16_t old_field = (uint16_t)((hdr[8] << 8) | hdr[9]);
        uint16_t new_field = (uint16_t)((ttl_new << 8) | hdr[9]);
        uint16_t inc = csum_update16(c, old_field, new_field);

        uint8_t h2[20];
        memcpy(h2, hdr, sizeof(h2));
        h2[8] = ttl_new;
        UT_ASSERT_EQ(inc, inet_csum(h2, sizeof(h2)));
    }
}

static void test_pseudo_header_matches_manual(void)
{
    /* csum_pseudo + payload must equal summing an explicit 12-byte
     * pseudo-header buffer (RFC 768 layout). */
    const uint32_t src = 0x0abe0001, dst = 0x0abe0002;
    const uint8_t proto = 17;
    const uint8_t payload[] = {0x12, 0x34, 0x00, 0x35, 0x00, 0x0b, 0x00, 0x00, 0xde, 0xad, 0xbe};
    const uint16_t plen = sizeof(payload);

    uint8_t manual[12 + sizeof(payload)];
    pf_put_be32(manual, src);
    pf_put_be32(manual + 4, dst);
    manual[8] = 0;
    manual[9] = proto;
    pf_put_be16(manual + 10, plen);
    memcpy(manual + 12, payload, plen);

    uint16_t via_helper =
        csum_fold(csum_partial(payload, plen, csum_pseudo(src, dst, proto, plen)));
    UT_ASSERT_EQ(via_helper, inet_csum(manual, sizeof(manual)));
}

int main(void)
{
    printf("test_csum:\n");
    UT_RUN(test_rfc1071_example);
    UT_RUN(test_odd_length_pads_right);
    UT_RUN(test_carry_folding);
    UT_RUN(test_verify_includes_checksum_field);
    UT_RUN(test_incremental_update_matches_recompute);
    UT_RUN(test_pseudo_header_matches_manual);
    printf("test_csum: all passed\n");
    return 0;
}
