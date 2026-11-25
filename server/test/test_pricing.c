#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "core/model.h"
#include "core/pricing.h"

#define L 1793919600LL /* Fri 2026-11-06 00:00 local */
#define H 3600LL

void setUp(void) {}
void tearDown(void) {}

static tariff_version_t cur(void) {
    tariff_version_t t;
    memset(&t, 0, sizeof t);
    t.rate_day_cents_per_h = 300; t.rate_night_cents_per_h = 150; t.rate_km_cents = 30;
    t.night_start = 1200; t.night_end = 360; t.unused_factor_permille = 500;
    t.time_cap_cents_per_day = 3000; t.combined_cap_cents_per_day = 5000;
    t.late_fee_cents_per_started_h = 1500;
    return t;
}
static tariff_version_t nocap(void) {
    tariff_version_t t = cur();
    t.time_cap_cents_per_day = 0; t.combined_cap_cents_per_day = 0;
    return t;
}

static void test_afternoon_40km(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L + 14 * H, L + 18 * H, L + 14 * H, L + 18 * H, 40, &p);
    TEST_ASSERT_EQUAL_INT(PRICE_OK, p.status);
    TEST_ASSERT_EQUAL_INT64(2400, p.total_cents);
    TEST_ASSERT_EQUAL_INT64(240, p.day_min);
    TEST_ASSERT_EQUAL_INT64(1200, p.time_cents);
    TEST_ASSERT_EQUAL_INT64(1200, p.km_cents);
}

static void test_early_return_unused(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L + 14 * H, L + 18 * H, L + 14 * H, L + 16 * H, 40, &p);
    TEST_ASSERT_EQUAL_INT64(2100, p.total_cents);
    TEST_ASSERT_EQUAL_INT64(120, p.unused_day_min);
}

static void test_full_day_both_caps(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L, L + 24 * H, L, L + 24 * H, 200, &p);
    TEST_ASSERT_EQUAL_INT64(3000, p.time_cents);
    TEST_ASSERT_EQUAL_INT64(2700, p.time_cap_cut_cents);
    TEST_ASSERT_EQUAL_INT64(6000, p.km_cents);
    TEST_ASSERT_EQUAL_INT64(4000, p.combined_cap_cut_cents);
    TEST_ASSERT_EQUAL_INT64(5000, p.total_cents);
    TEST_ASSERT_EQUAL_INT(1, p.days);
    TEST_ASSERT_EQUAL_INT64(840, p.day_min);
    TEST_ASSERT_EQUAL_INT64(600, p.night_min);
}

static void test_late_fee_tolerance(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L + 14 * H, L + 18 * H, L + 14 * H, L + 18 * H + 10 * 60, 0, &p);
    TEST_ASSERT_EQUAL_INT64(0, p.late_fee_cents);
    price_compute(&t, 15, L + 14 * H, L + 18 * H, L + 14 * H, L + 18 * H + 20 * 60, 0, &p);
    TEST_ASSERT_EQUAL_INT64(1500, p.late_fee_cents);
    price_compute(&t, 15, L + 14 * H, L + 18 * H, L + 14 * H, L + 19 * H + 20 * 60, 0, &p);
    TEST_ASSERT_EQUAL_INT64(3000, p.late_fee_cents);
    /* late fee sits outside the combined cap */
    price_compute(&t, 15, L, L + 24 * H, L, L + 24 * H + 20 * 60, 200, &p);
    TEST_ASSERT_EQUAL_INT64(1500, p.late_fee_cents);
    TEST_ASSERT_EQUAL_INT64(p.time_cents + p.km_cents - p.combined_cap_cut_cents + 1500, p.total_cents);
}

static void test_late_fee_truncates_seconds(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L + 14 * H, L + 18 * H, L + 14 * H, L + 18 * H + 15 * 60 + 59, 0, &p);
    TEST_ASSERT_EQUAL_INT64(0, p.late_fee_cents);
}

static void test_weekend_three_days(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L + 18 * H, L + 66 * H, L + 18 * H, L + 66 * H, 1000, &p);
    TEST_ASSERT_EQUAL_INT(3, p.days);
    TEST_ASSERT_EQUAL_INT64(15000, p.total_cents);
}

static void test_night_window(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L + 19 * H, L + 21 * H, L + 19 * H, L + 21 * H, 0, &p);
    TEST_ASSERT_EQUAL_INT64(60, p.day_min);
    TEST_ASSERT_EQUAL_INT64(60, p.night_min);
    TEST_ASSERT_EQUAL_INT64(450, p.time_cents);
}

static void test_spring_dst_day(void) {
    tariff_version_t t = nocap(); price_t p;
    int64_t s = 1774738800;
    price_compute(&t, 15, s, s + 23 * H, s, s + 23 * H, 0, &p);
    TEST_ASSERT_EQUAL_INT64(540, p.night_min);
    TEST_ASSERT_EQUAL_INT64(840, p.day_min);
    TEST_ASSERT_EQUAL_INT64(5550, p.time_cents);
    TEST_ASSERT_EQUAL_INT(1, p.days);
}

static void test_autumn_dst_day(void) {
    tariff_version_t t = nocap(); price_t p;
    int64_t s = 1792879200;
    price_compute(&t, 15, s, s + 25 * H, s, s + 25 * H, 0, &p);
    TEST_ASSERT_EQUAL_INT64(660, p.night_min);
    TEST_ASSERT_EQUAL_INT64(5850, p.time_cents);
    TEST_ASSERT_EQUAL_INT(1, p.days);
}

static void test_no_show_all_unused(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L + 14 * H, L + 18 * H, 0, 0, 0, &p);
    TEST_ASSERT_EQUAL_INT64(240, p.unused_day_min);
    TEST_ASSERT_EQUAL_INT64(0, p.day_min);
    TEST_ASSERT_EQUAL_INT64(600, p.total_cents);
}

static void test_early_start_counts_active(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L + 14 * H, L + 18 * H, L + 13 * H, L + 18 * H, 0, &p);
    TEST_ASSERT_EQUAL_INT64(300, p.day_min);
    TEST_ASSERT_EQUAL_INT64(0, p.unused_day_min);
    TEST_ASSERT_EQUAL_INT64(0, p.unused_night_min);
}

static void test_long_booking_31_days(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L, L + 31 * 24 * H, L, L + 31 * 24 * H, 5000, &p);
    TEST_ASSERT_EQUAL_INT(31, p.days);
    TEST_ASSERT_EQUAL_INT64(31 * 5000, p.total_cents);
}

static void test_rounding_once_per_day(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L, L + 60, L, L + 60, 0, &p); /* 1 active night min: 2.5 */
    TEST_ASSERT_EQUAL_INT64(3, p.time_cents);
}

static void test_rounding_unused(void) {
    tariff_version_t t = cur(); price_t p;
    price_compute(&t, 15, L, L + 60, 0, 0, 0, &p); /* 1 unused night min: 1.25 */
    TEST_ASSERT_EQUAL_INT64(1, p.time_cents);
}

static void test_cap_zero_means_none(void) {
    tariff_version_t t = nocap(); price_t p;
    price_compute(&t, 15, L, L + 24 * H, L, L + 24 * H, 200, &p);
    TEST_ASSERT_EQUAL_INT64(5700 + 6000, p.total_cents);
}

static booking_t bk(booking_state_t st) {
    booking_t b;
    memset(&b, 0, sizeof b);
    b.booked_start = L + 14 * H; b.booked_end = L + 18 * H; b.state = st;
    return b;
}
static const settings_t S = {15, 60, 500};

static void test_booking_cancelled(void) {
    booking_t b = bk(BK_CANCELLED); price_t p;
    b.cancel_fee_cents = 500;
    price_booking(NULL, &S, &b, &p);
    TEST_ASSERT_EQUAL_INT(PRICE_OK, p.status);
    TEST_ASSERT_EQUAL_INT64(500, p.total_cents);
    TEST_ASSERT_EQUAL_INT64(500, p.cancel_fee_cents);
}

static void test_booking_club_trip_cancelled(void) {
    booking_t b = bk(BK_CANCELLED); price_t p;
    b.club_trip = true; b.cancel_fee_cents = 500;
    price_booking(NULL, &S, &b, &p);
    TEST_ASSERT_EQUAL_INT(PRICE_OK, p.status);
    TEST_ASSERT_EQUAL_INT64(0, p.total_cents);
    TEST_ASSERT_EQUAL_INT64(0, p.cancel_fee_cents);
}

static void test_booking_club_trip(void) {
    tariff_version_t t = cur(); booking_t b = bk(BK_DONE); price_t p;
    b.club_trip = true; b.actual_start = b.booked_start; b.actual_end = b.booked_end + 2 * H;
    b.km = 100;
    price_booking(&t, &S, &b, &p);
    TEST_ASSERT_EQUAL_INT64(0, p.total_cents);
}

static void test_booking_done(void) {
    tariff_version_t t = cur(); booking_t b = bk(BK_DONE); price_t p;
    b.actual_start = b.booked_start; b.actual_end = b.booked_end;
    b.km = 40;
    price_booking(&t, &S, &b, &p);
    TEST_ASSERT_EQUAL_INT(PRICE_OK, p.status);
    TEST_ASSERT_EQUAL_INT64(2400, p.total_cents);
}

static void test_booking_km_pending(void) {
    tariff_version_t t = cur(); booking_t b = bk(BK_DONE); price_t p;
    b.actual_start = b.booked_start; b.actual_end = b.booked_end + 20 * 60;
    b.km_pending = true;
    price_booking(&t, &S, &b, &p);
    TEST_ASSERT_EQUAL_INT(PRICE_KM_PENDING, p.status);
    TEST_ASSERT_EQUAL_INT64(p.time_cents + p.late_fee_cents, p.total_cents);
    TEST_ASSERT_EQUAL_INT64(0, p.km_cents);
}

static void test_booking_no_tariff(void) {
    booking_t b = bk(BK_DONE); price_t p;
    b.actual_start = b.booked_start; b.actual_end = b.booked_end;
    price_booking(NULL, &S, &b, &p);
    TEST_ASSERT_EQUAL_INT(PRICE_NO_TARIFF, p.status);
    TEST_ASSERT_EQUAL_INT64(0, p.total_cents);
}

static void test_booking_done_never_started(void) {
    tariff_version_t t = cur(); booking_t b = bk(BK_DONE); price_t p;
    price_booking(&t, &S, &b, &p);
    TEST_ASSERT_EQUAL_INT64(240, p.unused_day_min);
    TEST_ASSERT_EQUAL_INT64(600, p.total_cents);
}

static void test_booking_done_missing_end(void) {
    tariff_version_t t = cur(); booking_t b = bk(BK_DONE); price_t p;
    b.actual_start = b.booked_start; /* actual_end == 0 */
    price_booking(&t, &S, &b, &p);
    TEST_ASSERT_EQUAL_INT64(240, p.day_min);
    TEST_ASSERT_EQUAL_INT64(0, p.late_fee_cents);
}

static void test_booking_reserved_estimate(void) {
    tariff_version_t t = cur(); booking_t b = bk(BK_RESERVED); price_t p;
    price_booking(&t, &S, &b, &p);
    TEST_ASSERT_EQUAL_INT(PRICE_KM_PENDING, p.status);
    TEST_ASSERT_EQUAL_INT64(1200, p.total_cents);
}

static void test_extreme_values_no_overflow(void) {
    tariff_version_t t = cur(); price_t p;
    t.rate_day_cents_per_h = t.rate_night_cents_per_h = 10000000; t.rate_km_cents = 10000000;
    t.time_cap_cents_per_day = t.combined_cap_cents_per_day = 0;
    price_compute(&t, 15, L, L + 730 * 24 * H, L, L + 730 * 24 * H, 10000000, &p);
    TEST_ASSERT_EQUAL_INT(730, p.days);
    TEST_ASSERT_EQUAL_INT64(730LL * 24 * 10000000 + 10000000LL * 10000000, p.total_cents);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_afternoon_40km);
    RUN_TEST(test_early_return_unused);
    RUN_TEST(test_full_day_both_caps);
    RUN_TEST(test_late_fee_tolerance);
    RUN_TEST(test_late_fee_truncates_seconds);
    RUN_TEST(test_weekend_three_days);
    RUN_TEST(test_night_window);
    RUN_TEST(test_spring_dst_day);
    RUN_TEST(test_autumn_dst_day);
    RUN_TEST(test_no_show_all_unused);
    RUN_TEST(test_early_start_counts_active);
    RUN_TEST(test_long_booking_31_days);
    RUN_TEST(test_rounding_once_per_day);
    RUN_TEST(test_rounding_unused);
    RUN_TEST(test_cap_zero_means_none);
    RUN_TEST(test_booking_cancelled);
    RUN_TEST(test_booking_club_trip_cancelled);
    RUN_TEST(test_booking_club_trip);
    RUN_TEST(test_booking_done);
    RUN_TEST(test_booking_km_pending);
    RUN_TEST(test_booking_no_tariff);
    RUN_TEST(test_booking_done_never_started);
    RUN_TEST(test_booking_done_missing_end);
    RUN_TEST(test_booking_reserved_estimate);
    RUN_TEST(test_extreme_values_no_overflow);
    return UNITY_END();
}
