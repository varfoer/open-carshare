#include <stdint.h>
#include "unity.h"
#include "core/km_resolve.h"

void setUp(void) {}
void tearDown(void) {}

#define S INT64_C(1800000000)
#define E (S + 7200)

static odometer_reading_t rd(int64_t car, int64_t ts, int64_t m) {
    odometer_reading_t r = {.car_id = car, .ts = ts, .odometer_m = m, .received_at = ts};
    return r;
}

static void test_resolve_basic(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 1000400), rd(1, E - 10, 1012600), rd(1, E + 3600, 1012600)};
    int64_t ks = -1, ke = -1;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 3, 1, S, E, &ks, &ke));
    TEST_ASSERT_EQUAL_INT64(1000, ks);
    TEST_ASSERT_EQUAL_INT64(1012, ke);
}

static void test_resolve_rounds_trip_once(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 1000499), rd(1, E - 10, 1001500), rd(1, E + 1, 1001500)};
    int64_t ks, ke;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 3, 1, S, E, &ks, &ke));
    TEST_ASSERT_EQUAL_INT64(1000, ks);
    TEST_ASSERT_EQUAL_INT64(1001, ke);
}

static void test_resolve_wait_without_later_reading(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 1000400), rd(1, E, 1012600)};
    int64_t ks, ke;
    TEST_ASSERT_EQUAL_INT(KMR_WAIT, km_resolve(r, 2, 1, S, E, &ks, &ke));
    TEST_ASSERT_EQUAL_INT(KMR_WAIT, km_resolve(r, 0, 1, S, E, &ks, &ke));
}

static void test_resolve_out_of_order_arrival(void) {
    odometer_reading_t r[] = {rd(1, E + 3600, 1012600), rd(1, S + 10, 1000400),
                              rd(1, E - 10, 1012600), rd(1, E - 300, 1011000)};
    int64_t ks, ke;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 4, 1, S, E, &ks, &ke));
    TEST_ASSERT_EQUAL_INT64(1000, ks);
    TEST_ASSERT_EQUAL_INT64(1012, ke);
}

static void test_resolve_zero_km_parked(void) {
    odometer_reading_t r[] = {rd(1, S - 100, 5000000), rd(1, E + 100, 5000000)};
    int64_t ks, ke;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 2, 1, S, E, &ks, &ke));
    TEST_ASSERT_EQUAL_INT64(5000, ks);
    TEST_ASSERT_EQUAL_INT64(5000, ke);
}

static void test_resolve_gap_bad(void) {
    odometer_reading_t r[] = {rd(1, S - 100, 5000000), rd(1, E + 100, 5003000)};
    int64_t ks, ke;
    TEST_ASSERT_EQUAL_INT(KMR_BAD, km_resolve(r, 2, 1, S, E, &ks, &ke));
}

static void test_resolve_backwards_bad(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 2000000), rd(1, E - 10, 1000000), rd(1, E + 10, 1000000)};
    int64_t ks, ke;
    TEST_ASSERT_EQUAL_INT(KMR_BAD, km_resolve(r, 3, 1, S, E, &ks, &ke));
}

static void test_resolve_over_limit_bad(void) {
    odometer_reading_t r[] = {rd(1, S + 10, 9999000000LL), rd(1, E - 10, 10001000000LL), rd(1, E + 1, 1)};
    int64_t ks, ke;
    TEST_ASSERT_EQUAL_INT(KMR_BAD, km_resolve(r, 3, 1, S, E, &ks, &ke));
}

static void test_resolve_ignores_other_car(void) {
    odometer_reading_t r[] = {rd(2, S + 10, 1), rd(2, E - 10, 99000000), rd(2, E + 5, 99000000),
                              rd(1, S + 20, 1000000), rd(1, E - 20, 1010000), rd(1, E + 5, 1010000)};
    int64_t ks, ke;
    TEST_ASSERT_EQUAL_INT(KMR_OK, km_resolve(r, 6, 1, S, E, &ks, &ke));
    TEST_ASSERT_EQUAL_INT64(1000, ks);
    TEST_ASSERT_EQUAL_INT64(1010, ke);
    odometer_reading_t o[] = {rd(2, E + 5, 1)};
    TEST_ASSERT_EQUAL_INT(KMR_WAIT, km_resolve(o, 1, 1, S, E, &ks, &ke));
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
    return UNITY_END();
}
