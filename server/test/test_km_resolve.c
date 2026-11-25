#include <stdint.h>
#include "unity.h"
#include "core/km_resolve.h"

void setUp(void) {}
void tearDown(void) {}

#define S INT64_C(1800000000)
#define E (S + 7200)

#define SETTLED (E + 100000) /* every rd() reading (received_at == ts) counts as settled */

static odometer_reading_t rd(int64_t car, int64_t ts, int64_t m) {
    odometer_reading_t r = {.car_id = car, .ts = ts, .odometer_m = m, .received_at = ts};
    return r;
}

static void test_resolve_basic(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 1000400), rd(1, E - 10, 1012600), rd(1, E + 3600, 1012600)};
    int64_t km = -1;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 3, 1, S, E, SETTLED, &km));
    TEST_ASSERT_EQUAL_INT64(12, km);
}

static void test_resolve_rounds_trip_once(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 1000499), rd(1, E - 10, 1001500), rd(1, E + 1, 1001500)};
    int64_t km;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 3, 1, S, E, SETTLED, &km));
    TEST_ASSERT_EQUAL_INT64(1, km); /* 1001.1 km rounded once */
}

static void test_resolve_wait_without_later_reading(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 1000400), rd(1, E, 1012600)};
    int64_t km;
    TEST_ASSERT_EQUAL_INT(KMR_WAIT, km_resolve(r, 2, 1, S, E, SETTLED, &km));
    TEST_ASSERT_EQUAL_INT(KMR_WAIT, km_resolve(r, 0, 1, S, E, SETTLED, &km));
}

static void test_resolve_out_of_order_arrival(void) {
    odometer_reading_t r[] = {rd(1, E + 3600, 1012600), rd(1, S + 10, 1000400),
                              rd(1, E - 10, 1012600), rd(1, E - 300, 1011000)};
    int64_t km;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 4, 1, S, E, SETTLED, &km));
    TEST_ASSERT_EQUAL_INT64(12, km);
}

static void test_resolve_zero_km_parked(void) {
    odometer_reading_t r[] = {rd(1, S - 100, 5000000), rd(1, E + 100, 5000000)};
    int64_t km;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 2, 1, S, E, SETTLED, &km));
    TEST_ASSERT_EQUAL_INT64(0, km);
}

static void test_resolve_gap_bad(void) {
    odometer_reading_t r[] = {rd(1, S - 100, 5000000), rd(1, E + 100, 5003000)};
    int64_t km;
    TEST_ASSERT_EQUAL_INT(KMR_BAD, km_resolve(r, 2, 1, S, E, SETTLED, &km));
}

static void test_resolve_backwards_bad(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 2000000), rd(1, E - 10, 1000000), rd(1, E + 10, 1000000)};
    int64_t km;
    TEST_ASSERT_EQUAL_INT(KMR_BAD, km_resolve(r, 3, 1, S, E, SETTLED, &km));
}

static void test_resolve_over_limit_bad(void) {
    /* Only the trip distance is limited (10000000 km), not the odometer reading. */
    odometer_reading_t r[] = {rd(1, S + 10, 0), rd(1, E - 10, 10000001000LL), rd(1, E + 1, 10000001000LL)};
    int64_t km;
    TEST_ASSERT_EQUAL_INT(KMR_BAD, km_resolve(r, 3, 1, S, E, SETTLED, &km));
    odometer_reading_t big[] = {rd(1, S + 10, 9999000000LL), rd(1, E - 10, 10001000000LL), rd(1, E + 1, 1)};
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(big, 3, 1, S, E, SETTLED, &km));
    TEST_ASSERT_EQUAL_INT64(2000, km);
}

static void test_resolve_ignores_other_car(void) {
    odometer_reading_t r[] = {rd(2, S + 10, 1), rd(2, E - 10, 99000000), rd(2, E + 5, 99000000),
                              rd(1, S + 20, 1000000), rd(1, E - 20, 1010000), rd(1, E + 5, 1010000)};
    int64_t km;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 6, 1, S, E, SETTLED, &km));
    TEST_ASSERT_EQUAL_INT64(10, km);
    odometer_reading_t o[] = {rd(2, E + 5, 1)};
    TEST_ASSERT_EQUAL_INT(KMR_WAIT, km_resolve(o, 1, 1, S, E, SETTLED, &km));
}

static void test_resolve_recent_later_reading_waits(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 1000400), rd(1, E - 10, 1012600), rd(1, E + 100, 1012600)};
    r[2].received_at = 5000;
    int64_t km = -1;
    TEST_ASSERT_EQUAL_INT(KMR_WAIT, km_resolve(r, 3, 1, S, E, 4999, &km));
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 3, 1, S, E, 5000, &km));
    TEST_ASSERT_EQUAL_INT64(12, km);
}

static void test_live_first_to_last_in_window(void) {
    odometer_reading_t r[] = {rd(1, S - 100, 999000), rd(1, S + 300, 1000499), rd(2, S + 400, 7000000),
                              rd(1, S + 900, 1023000), rd(1, S + 600, 1010000), rd(1, E + 100, 1030000)};
    int64_t km = -1, as_of = -1;
    TEST_ASSERT_TRUE(km_live(r, 6, 1, S, S + 1000, &km, &as_of));
    TEST_ASSERT_EQUAL_INT64(23, km); /* 22501 m, half-up */
    TEST_ASSERT_EQUAL_INT64(S + 900, as_of);
    TEST_ASSERT_TRUE(km_live(r, 6, 1, S, S + 300, &km, &as_of)); /* one reading: 0 km */
    TEST_ASSERT_EQUAL_INT64(0, km);
    TEST_ASSERT_EQUAL_INT64(S + 300, as_of);
}

static void test_live_none(void) {
    odometer_reading_t r[] = {rd(1, S - 100, 1000000), rd(1, S + 300, 1005000), rd(1, S + 600, 1004000),
                              rd(1, E + 100, 1010000)};
    int64_t km = -1, as_of = -1;
    TEST_ASSERT_FALSE(km_live(r, 4, 1, S + 700, E, &km, &as_of)); /* no reading in the window */
    TEST_ASSERT_FALSE(km_live(r, 4, 1, S, S + 700, &km, &as_of)); /* odometer went backwards */
    TEST_ASSERT_FALSE(km_live(r, 4, 2, S, E, &km, &as_of));       /* other car */
    TEST_ASSERT_FALSE(km_live(r, 0, 1, S, E, &km, &as_of));
    odometer_reading_t big[] = {rd(1, S + 1, 0), rd(1, S + 2, INT64_C(20000000000000))};
    TEST_ASSERT_FALSE(km_live(big, 2, 1, S, E, &km, &as_of));     /* implausible */
    TEST_ASSERT_EQUAL_INT64(-1, km);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_resolve_basic);
    RUN_TEST(test_resolve_rounds_trip_once);
    RUN_TEST(test_resolve_wait_without_later_reading);
    RUN_TEST(test_resolve_out_of_order_arrival);
    RUN_TEST(test_resolve_zero_km_parked);
    RUN_TEST(test_resolve_gap_bad);
    RUN_TEST(test_resolve_backwards_bad);
    RUN_TEST(test_resolve_over_limit_bad);
    RUN_TEST(test_resolve_ignores_other_car);
    RUN_TEST(test_resolve_recent_later_reading_waits);
    RUN_TEST(test_live_first_to_last_in_window);
    RUN_TEST(test_live_none);
    return UNITY_END();
}
