#include <string.h>
#include "unity.h"
#include "core/model.h"
#include "core/billing.h"

void setUp(void) {}
void tearDown(void) {}

static void test_period_parse(void) {
    int y = 0, m = 0;
    TEST_ASSERT_EQUAL_INT(0, billing_period_parse("2026-10", &y, &m));
    TEST_ASSERT_EQUAL_INT(2026, y);
    TEST_ASSERT_EQUAL_INT(10, m);
    TEST_ASSERT_EQUAL_INT(-1, billing_period_parse("2026-13", &y, &m));
    TEST_ASSERT_EQUAL_INT(-1, billing_period_parse("2026-00", &y, &m));
    TEST_ASSERT_EQUAL_INT(-1, billing_period_parse("26-10", &y, &m));
    TEST_ASSERT_EQUAL_INT(-1, billing_period_parse("2026-1", &y, &m));
    TEST_ASSERT_EQUAL_INT(-1, billing_period_parse("2026-10x", &y, &m));
    TEST_ASSERT_EQUAL_INT(-1, billing_period_parse("1999-12", &y, &m));
    TEST_ASSERT_EQUAL_INT(-1, billing_period_parse("2100-01", &y, &m));
    TEST_ASSERT_EQUAL_INT(-1, billing_period_parse("", &y, &m));
}

static void test_period_bounds_local(void) {
    int64_t from, to;
    billing_period_bounds(2026, 10, &from, &to);
    TEST_ASSERT_EQUAL_INT64(1790805600LL, from);
    TEST_ASSERT_EQUAL_INT64(1793487600LL, to);
    billing_period_bounds(2026, 12, &from, &to);
    TEST_ASSERT_EQUAL_INT64(1793487600LL + 30 * 86400LL, from);
    char d[11];
    billing_local_date(to, d);
    TEST_ASSERT_EQUAL_STRING("2027-01-01", d);
}

static booking_t done(int64_t end) {
    booking_t b; memset(&b, 0, sizeof b);
    b.id = 1; b.state = BK_DONE; b.booked_start = end - 7200; b.booked_end = end;
    b.actual_start = end - 7200; b.actual_end = end;
    return b;
}

static void test_includes_month_end_local(void) {
    int64_t from, to;
    billing_period_bounds(2026, 10, &from, &to);
    booking_t b = done(1793485800LL);
    TEST_ASSERT_TRUE(billing_includes(&b, to));
    b = done(1793487600LL);
    TEST_ASSERT_FALSE(billing_includes(&b, to));
}

static void test_includes_rules(void) {
    int64_t to = 1793487600LL;
    booking_t b = done(1793485800LL);
    b.invoice_id = 5;
    TEST_ASSERT_FALSE(billing_includes(&b, to));
    b.invoice_id = 0;
    b.state = BK_RESERVED; TEST_ASSERT_FALSE(billing_includes(&b, to));
    b.state = BK_ACTIVE; TEST_ASSERT_FALSE(billing_includes(&b, to));
    b = done(1793485800LL);
    b.state = BK_CANCELLED; b.cancelled_at = 1793485800LL; b.cancel_fee_cents = 0;
    TEST_ASSERT_FALSE(billing_includes(&b, to));
    b.cancel_fee_cents = 300;
    TEST_ASSERT_TRUE(billing_includes(&b, to));
    TEST_ASSERT_EQUAL_INT64(1793485800LL, billing_bill_time(&b));
    b.club_trip = true;
    TEST_ASSERT_FALSE(billing_includes(&b, to));
    /* never started DONE uses booked_end */
    b = done(1793485800LL);
    b.actual_start = b.actual_end = 0;
    TEST_ASSERT_EQUAL_INT64(1793485800LL, billing_bill_time(&b));
    TEST_ASSERT_TRUE(billing_includes(&b, to));
    b.booked_end = to + 10;
    TEST_ASSERT_FALSE(billing_includes(&b, to));
    /* straggler from an earlier month */
    b = done(1790805600LL - 86400);
    TEST_ASSERT_TRUE(billing_includes(&b, to));
    /* bill time 0 never included */
    b = done(0); b.booked_end = 0;
    TEST_ASSERT_FALSE(billing_includes(&b, to));
}

static void test_target2_days(void) {
    TEST_ASSERT_FALSE(target2_open(2027, 3, 26));
    TEST_ASSERT_FALSE(target2_open(2027, 3, 29));
    TEST_ASSERT_TRUE(target2_open(2027, 3, 30));
    TEST_ASSERT_FALSE(target2_open(2026, 5, 1));
    TEST_ASSERT_FALSE(target2_open(2026, 12, 25));
    TEST_ASSERT_FALSE(target2_open(2026, 12, 26));
    TEST_ASSERT_FALSE(target2_open(2027, 1, 1));
    TEST_ASSERT_FALSE(target2_open(2026, 11, 7));  /* Sat */
    TEST_ASSERT_FALSE(target2_open(2026, 11, 8));  /* Sun */
    TEST_ASSERT_TRUE(target2_open(2026, 11, 9));   /* Mon */
    TEST_ASSERT_TRUE(target2_open(2026, 10, 2));   /* Fri */
}

static void test_collection_date_skips_easter_monday(void) {
    char d[11];
    billing_collection_date(1805878800LL, 5, d);
    TEST_ASSERT_EQUAL_STRING("2027-03-30", d);
}

static void test_collection_date_local_midnight(void) {
    char d[11];
    billing_collection_date(1793835000LL, 5, d);
    TEST_ASSERT_EQUAL_STRING("2026-11-10", d);
    billing_collection_date(1793658600LL, 5, d);
    TEST_ASSERT_EQUAL_STRING("2026-11-09", d);
}

static void test_line_from_price(void) {
    booking_t b = done(1793485800LL);
    b.id = 42; b.actual_start = 1793480000LL; b.booked_start = 1793470000LL;
    price_t p; memset(&p, 0, sizeof p);
    p.status = PRICE_OK; p.day_min = 100; p.night_min = 20;
    p.unused_day_min = 7; p.unused_night_min = 5;
    p.time_cents = 900; p.time_cap_cut_cents = 30; p.combined_cap_cut_cents = 12;
    p.km = 33; p.km_cents = 990; p.late_fee_cents = 1500; p.cancel_fee_cents = 0;
    p.total_cents = 3390;
    invoice_line_t l;
    billing_line(&b, "Zoe", &p, &l);
    TEST_ASSERT_EQUAL_INT64(0, l.invoice_number);
    TEST_ASSERT_EQUAL_INT64(42, l.booking_id);
    TEST_ASSERT_EQUAL_STRING("Zoe", l.car_name);
    TEST_ASSERT_EQUAL_INT64(1793480000LL, l.start);
    TEST_ASSERT_EQUAL_INT64(1793485800LL, l.end);
    TEST_ASSERT_FALSE(l.never_started);
    TEST_ASSERT_FALSE(l.cancelled);
    TEST_ASSERT_FALSE(l.club_trip);
    TEST_ASSERT_EQUAL_INT64(100, l.day_min);
    TEST_ASSERT_EQUAL_INT64(20, l.night_min);
    TEST_ASSERT_EQUAL_INT64(12, l.unused_min);
    TEST_ASSERT_EQUAL_INT64(33, l.km);
    TEST_ASSERT_EQUAL_INT64(900, l.time_cents);
    TEST_ASSERT_EQUAL_INT64(990, l.km_cents);
    TEST_ASSERT_EQUAL_INT64(42, l.cap_cut_cents);
    TEST_ASSERT_EQUAL_INT64(1500, l.late_fee_cents);
    TEST_ASSERT_EQUAL_INT64(0, l.cancel_fee_cents);
    TEST_ASSERT_EQUAL_INT64(3390, l.total_cents);
    /* never started: booked times; unknown km -> 0 */
    b.actual_start = b.actual_end = 0;
    b.booked_start = 1000; b.booked_end = 2000;
    p.km = -1;
    billing_line(&b, "Zoe", &p, &l);
    TEST_ASSERT_TRUE(l.never_started);
    TEST_ASSERT_EQUAL_INT64(1000, l.start);
    TEST_ASSERT_EQUAL_INT64(2000, l.end);
    TEST_ASSERT_EQUAL_INT64(0, l.km);
}

static void test_line_cancelled_and_club(void) {
    booking_t b = done(5000);
    b.state = BK_CANCELLED; b.actual_start = b.actual_end = 0;
    b.booked_start = 1000; b.booked_end = 2000; b.cancelled_at = 900;
    price_t p; memset(&p, 0, sizeof p);
    p.cancel_fee_cents = 300; p.total_cents = 300;
    invoice_line_t l;
    billing_line(&b, "Zoe", &p, &l);
    TEST_ASSERT_TRUE(l.cancelled);
    TEST_ASSERT_FALSE(l.never_started);
    TEST_ASSERT_EQUAL_INT64(1000, l.start);
    TEST_ASSERT_EQUAL_INT64(2000, l.end);
    TEST_ASSERT_EQUAL_INT64(300, l.total_cents);
    /* club trip: km from booking, money 0 */
    b = done(5000);
    b.club_trip = true; b.km_start = 100; b.km_end = 160; b.km_pending = false;
    memset(&p, 0, sizeof p);
    billing_line(&b, "Zoe", &p, &l);
    TEST_ASSERT_TRUE(l.club_trip);
    TEST_ASSERT_EQUAL_INT64(60, l.km);
    TEST_ASSERT_EQUAL_INT64(0, l.total_cents);
    b.km_pending = true;
    billing_line(&b, "Zoe", &p, &l);
    TEST_ASSERT_EQUAL_INT64(0, l.km);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_period_parse);
    RUN_TEST(test_period_bounds_local);
    RUN_TEST(test_includes_month_end_local);
    RUN_TEST(test_includes_rules);
    RUN_TEST(test_target2_days);
    RUN_TEST(test_collection_date_skips_easter_monday);
    RUN_TEST(test_collection_date_local_midnight);
    RUN_TEST(test_line_from_price);
    RUN_TEST(test_line_cancelled_and_club);
    return UNITY_END();
}
