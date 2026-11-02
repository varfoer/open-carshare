#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "helpers.h"
#include "core/teltonika.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t wiki[] = {
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x4A,
    0x8E, 0x01,
    0x00,0x00,0x01,0x6B,0x41,0x2C,0xEE,0x00, 0x01,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0x00,0x01, 0x00,0x05,
    0x00,0x01, 0x00,0x01,0x01,
    0x00,0x01, 0x00,0x11,0x00,0x1D,
    0x00,0x01, 0x00,0x10,0x01,0x5E,0x2C,0x88,
    0x00,0x02, 0x00,0x0B,0x00,0x00,0x00,0x00,0x35,0x44,0xC8,0x7A,
               0x00,0x0E,0x00,0x00,0x00,0x00,0x1D,0xD7,0xE0,0x6A,
    0x00,0x00,
    0x01,
    0x00,0x00,0x29,0x94};

static void test_wiki_example_packet(void) {
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n = 99, used = 0;
    TEST_ASSERT_EQUAL_UINT(86, sizeof wiki);
    TEST_ASSERT_EQUAL_INT(1, tlt_parse_packet(wiki, sizeof wiki, recs, &n, &used));
    TEST_ASSERT_EQUAL_UINT(86, used);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_INT64(1560166592000LL, recs[0].ts_ms);
    TEST_ASSERT_TRUE(recs[0].has_odometer);
    TEST_ASSERT_EQUAL_UINT32(22949000, recs[0].odometer_m);
}

static void test_crc16_wiki_example(void) {
    TEST_ASSERT_EQUAL_HEX16(0x2994, tlt_crc16(wiki + 8, 74));
}

static void test_packet_need_more(void) {
    tlt_record_t recs[TLT_MAX_RECORDS];
    for (size_t l = 0; l < sizeof wiki; l++) {
        size_t n = 0, used = 0;
        TEST_ASSERT_EQUAL_INT(0, tlt_parse_packet(wiki, l, recs, &n, &used));
    }
}

static int parse_mut(size_t off, uint8_t v) {
    uint8_t b[sizeof wiki];
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n, used;
    memcpy(b, wiki, sizeof b);
    b[off] = v;
    return tlt_parse_packet(b, sizeof b, recs, &n, &used);
}

static void test_packet_bad_crc(void) { TEST_ASSERT_EQUAL_INT(-1, parse_mut(85, 0x95)); }
static void test_packet_bad_codec(void) { TEST_ASSERT_EQUAL_INT(-1, parse_mut(8, 0x08)); }
static void test_packet_n1_ne_n2(void) { TEST_ASSERT_EQUAL_INT(-1, parse_mut(81, 0x02)); }
static void test_packet_nonzero_preamble(void) { TEST_ASSERT_EQUAL_INT(-1, parse_mut(2, 0x01)); }

static void test_packet_preamble_rejected_early(void) {
    uint8_t b[3] = {0, 0, 1};
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n, used;
    TEST_ASSERT_EQUAL_INT(-1, tlt_parse_packet(b, sizeof b, recs, &n, &used));
}

static void test_packet_length_too_big(void) {
    uint8_t h[8] = {0, 0, 0, 0, 0, 0, 0x40, 0x01}; /* 16385 */
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n, used;
    TEST_ASSERT_EQUAL_INT(-1, tlt_parse_packet(h, sizeof h, recs, &n, &used));
}

static void test_packet_length_too_small(void) {
    uint8_t h[8] = {0, 0, 0, 0, 0, 0, 0, 2};
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n, used;
    TEST_ASSERT_EQUAL_INT(-1, tlt_parse_packet(h, sizeof h, recs, &n, &used));
}

static void test_packet_huge_length_no_overflow(void) {
    uint8_t h[8] = {0, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF};
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n, used;
    TEST_ASSERT_EQUAL_INT(-1, tlt_parse_packet(h, sizeof h, recs, &n, &used));
}

static void test_packet_record_without_odometer(void) {
    uint8_t b[256];
    int64_t ts[1] = {1700000000000LL}, odo[1] = {-1};
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n = 0, used = 0, len = th_tlt_packet(b, sizeof b, ts, odo, 1);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_INT(1, tlt_parse_packet(b, len, recs, &n, &used));
    TEST_ASSERT_EQUAL_UINT(len, used);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_FALSE(recs[0].has_odometer);
    TEST_ASSERT_EQUAL_INT64(1700000000000LL, recs[0].ts_ms);
}

static void test_packet_multi_records_helper(void) {
    uint8_t b[512];
    int64_t ts[3] = {1000, 2000, 3000}, odo[3] = {5, -1, 4000000000LL};
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n = 0, used = 0, len = th_tlt_packet(b, sizeof b, ts, odo, 3);
    TEST_ASSERT_EQUAL_INT(1, tlt_parse_packet(b, len, recs, &n, &used));
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_TRUE(recs[0].has_odometer);
    TEST_ASSERT_EQUAL_UINT32(5, recs[0].odometer_m);
    TEST_ASSERT_FALSE(recs[1].has_odometer);
    TEST_ASSERT_EQUAL_UINT32(4000000000u, recs[2].odometer_m);
    TEST_ASSERT_EQUAL_INT64(3000, recs[2].ts_ms);
    TEST_ASSERT_EQUAL_size_t(0, th_tlt_packet(b, 10, ts, odo, 3));
}

static void test_packet_trailing_bytes_not_used(void) {
    uint8_t b[sizeof wiki + 5];
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n, used;
    memcpy(b, wiki, sizeof wiki);
    memset(b + sizeof wiki, 0xAA, 5);
    TEST_ASSERT_EQUAL_INT(1, tlt_parse_packet(b, sizeof b, recs, &n, &used));
    TEST_ASSERT_EQUAL_UINT(86, used);
}

/* One record: IO 16 as N4 plus one NX element (id 0x0100, len 3). */
static void test_packet_nx_group_skipped(void) {
    uint8_t d[] = {
        0x8E, 0x01,
        0,0,0,0,0,0,0x03,0xE8, 0x01,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0x00,0x00, 0x00,0x02,
        0x00,0x00, 0x00,0x00, 0x00,0x01, 0x00,0x10,0x00,0x00,0x03,0xE8, 0x00,0x00,
        0x00,0x01, 0x01,0x00, 0x00,0x03, 0xAA,0xBB,0xCC,
        0x01};
    uint8_t b[8 + sizeof d + 4] = {0};
    b[6] = (uint8_t)(sizeof d >> 8);
    b[7] = (uint8_t)sizeof d;
    memcpy(b + 8, d, sizeof d);
    uint16_t c = tlt_crc16(d, sizeof d);
    b[8 + sizeof d + 2] = (uint8_t)(c >> 8);
    b[8 + sizeof d + 3] = (uint8_t)c;
    tlt_record_t recs[TLT_MAX_RECORDS];
    size_t n = 0, used = 0;
    TEST_ASSERT_EQUAL_INT(1, tlt_parse_packet(b, sizeof b, recs, &n, &used));
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_TRUE(recs[0].has_odometer);
    TEST_ASSERT_EQUAL_UINT32(1000, recs[0].odometer_m);
    TEST_ASSERT_EQUAL_INT64(1000, recs[0].ts_ms);
}

static void imei_msg(uint8_t *b, uint16_t len, const char *digits) {
    b[0] = (uint8_t)(len >> 8);
    b[1] = (uint8_t)len;
    memcpy(b + 2, digits, 15);
}

static void test_imei_ok(void) {
    uint8_t b[17];
    char imei[16];
    size_t used = 0;
    imei_msg(b, 15, "356307042441013");
    TEST_ASSERT_EQUAL_INT(1, tlt_parse_imei(b, 17, imei, &used));
    TEST_ASSERT_EQUAL_UINT(17, used);
    TEST_ASSERT_EQUAL_STRING("356307042441013", imei);
}

static void test_imei_need_more(void) {
    uint8_t b[17];
    char imei[16];
    size_t used;
    imei_msg(b, 15, "356307042441013");
    for (size_t l = 0; l < 17; l++)
        TEST_ASSERT_EQUAL_INT(0, tlt_parse_imei(b, l, imei, &used));
}

static void test_imei_bad_len(void) {
    uint8_t b[17];
    char imei[16];
    size_t used;
    imei_msg(b, 14, "356307042441013");
    TEST_ASSERT_EQUAL_INT(-1, tlt_parse_imei(b, 17, imei, &used));
    TEST_ASSERT_EQUAL_INT(-1, tlt_parse_imei(b, 2, imei, &used));
}

static void test_imei_non_digit(void) {
    uint8_t b[17];
    char imei[16];
    size_t used;
    imei_msg(b, 15, "35630704244101x");
    TEST_ASSERT_EQUAL_INT(-1, tlt_parse_imei(b, 17, imei, &used));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wiki_example_packet);
    RUN_TEST(test_crc16_wiki_example);
    RUN_TEST(test_packet_need_more);
    RUN_TEST(test_packet_bad_crc);
    RUN_TEST(test_packet_bad_codec);
    RUN_TEST(test_packet_n1_ne_n2);
    RUN_TEST(test_packet_nonzero_preamble);
    RUN_TEST(test_packet_preamble_rejected_early);
    RUN_TEST(test_packet_length_too_big);
    RUN_TEST(test_packet_length_too_small);
    RUN_TEST(test_packet_huge_length_no_overflow);
    RUN_TEST(test_packet_record_without_odometer);
    RUN_TEST(test_packet_multi_records_helper);
    RUN_TEST(test_packet_trailing_bytes_not_used);
    RUN_TEST(test_packet_nx_group_skipped);
    RUN_TEST(test_imei_ok);
    RUN_TEST(test_imei_need_more);
    RUN_TEST(test_imei_bad_len);
    RUN_TEST(test_imei_non_digit);
    return UNITY_END();
}
