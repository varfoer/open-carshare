#include <stdint.h>
#include "unity.h"
#include "core/berlin.h"

void setUp(void) {}
void tearDown(void) {}

static void test_offset_around_spring_change(void) {
    TEST_ASSERT_EQUAL_INT64(3600, berlin_offset(1774745999));
    TEST_ASSERT_EQUAL_INT64(7200, berlin_offset(1774746000)); /* 2026-03-29 01:00 UTC */
}

static void test_offset_around_autumn_change(void) {
    TEST_ASSERT_EQUAL_INT64(7200, berlin_offset(1792889999));
    TEST_ASSERT_EQUAL_INT64(3600, berlin_offset(1792890000)); /* 2026-10-25 01:00 UTC */
}

static void test_day_start_winter(void) {
    TEST_ASSERT_EQUAL_INT64(1793919600, berlin_day_start(1793923200)); /* 01:00 local */
    TEST_ASSERT_EQUAL_INT64(1793919600, berlin_day_start(1793919600));
}

static void test_short_and_long_day(void) {
    TEST_ASSERT_EQUAL_INT64(23 * 3600, berlin_next_day_start(1774738800) - 1774738800);
    TEST_ASSERT_EQUAL_INT64(25 * 3600, berlin_next_day_start(1792879200) - 1792879200);
    TEST_ASSERT_EQUAL_INT64(24 * 3600, berlin_next_day_start(1793919600) - 1793919600);
}

static void test_other_years(void) {
    /* 2027: 28 Mar / 31 Oct, 2030: 31 Mar / 27 Oct, all 01:00 UTC */
    int64_t c27m = 1806195600, c27o = 1824944400, c30m = 1901149200, c30o = 1919293200;
    TEST_ASSERT_EQUAL_INT64(3600, berlin_offset(c27m - 1));
    TEST_ASSERT_EQUAL_INT64(7200, berlin_offset(c27m));
    TEST_ASSERT_EQUAL_INT64(7200, berlin_offset(c27o - 1));
    TEST_ASSERT_EQUAL_INT64(3600, berlin_offset(c27o));
    TEST_ASSERT_EQUAL_INT64(3600, berlin_offset(c30m - 1));
    TEST_ASSERT_EQUAL_INT64(7200, berlin_offset(c30m));
    TEST_ASSERT_EQUAL_INT64(7200, berlin_offset(c30o - 1));
    TEST_ASSERT_EQUAL_INT64(3600, berlin_offset(c30o));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_offset_around_spring_change);
    RUN_TEST(test_offset_around_autumn_change);
    RUN_TEST(test_day_start_winter);
    RUN_TEST(test_short_and_long_day);
    RUN_TEST(test_other_years);
    return UNITY_END();
}
