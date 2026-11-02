#include <stdio.h>
#include <string.h>

#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400
#define H 3600

static app_t *a;
static int64_t car1, car2, user1;

static int64_t add_car(const char *name, const char *imei) {
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "%s", name);
  snprintf(c.plate, sizeof c.plate, "X-%s", name);
  snprintf(c.location, sizeof c.location, "Hof");
  snprintf(c.tracker_imei, sizeof c.tracker_imei, "%s", imei);
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  return c.id;
}

static int64_t add_pending(int64_t car, int64_t start, int64_t end, bool pending) {
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = car;
  b.user_id = user1;
  b.booked_start = start;
  b.booked_end = end;
  b.state = BK_DONE;
  b.actual_start = start;
  b.actual_end = end;
  b.km_pending = pending;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  return b.id;
}

static void add_readings(int64_t car, int64_t start, int64_t end) {
  odometer_reading_t r[3] = {
    {car, start + 60, 1000000, T}, {car, end - 60, 1012000, T}, {car, end + 100, 1012000, T}};
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&a->db, r, 3));
}

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  user1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(user1 > 0);
  TEST_ASSERT_TRUE(th_add_user(a, "admin@x.de", NULL, ROLE_ADMIN) > 0);
  int64_t off = th_add_user(a, "off@x.de", NULL, ROLE_ADMIN);
  TEST_ASSERT_TRUE(off > 0);
  user_t u = *db_user(&a->db, off);
  u.active = false;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
  car1 = add_car("Golf", "123456789012345");
  car2 = add_car("Polo", "");
}

void tearDown(void) { th_free(a); }

static void test_resolve_sets_telematics_km(void) {
  int64_t id = add_pending(car1, T, T + 2 * H, true);
  add_readings(car1, T, T + 2 * H);
  TEST_ASSERT_EQUAL_INT(1, api_resolve_km(a, T + 3 * H));
  const booking_t *b = db_booking(&a->db, id);
  TEST_ASSERT_EQUAL_INT64(1000, b->km_start);
  TEST_ASSERT_EQUAL_INT64(1012, b->km_end);
  TEST_ASSERT_EQUAL_INT(KM_TELEMATICS, b->km_source);
  TEST_ASSERT_FALSE(b->km_pending);
  TEST_ASSERT_EQUAL_INT64(T + 3 * H, b->modified_at);
  TEST_ASSERT_EQUAL_INT64(0, b->modified_by);
  char dir[256];
  snprintf(dir, sizeof dir, "%s", a->cfg.data_dir);
  db_free(&a->db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&a->db, dir));
  b = db_booking(&a->db, id);
  TEST_ASSERT_EQUAL_INT64(1012, b->km_end);
  TEST_ASSERT_EQUAL_INT(KM_TELEMATICS, b->km_source);
  TEST_ASSERT_FALSE(b->km_pending);
}

static void test_resolve_waits_without_later_reading(void) {
  int64_t id = add_pending(car1, T, T + 2 * H, true);
  odometer_reading_t r[1] = {{car1, T + 60, 1000000, T}};
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&a->db, r, 1));
  TEST_ASSERT_EQUAL_INT(0, api_resolve_km(a, T + 3 * H));
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->km_pending);
}

static void test_resolve_skips_car_without_tracker(void) {
  int64_t id = add_pending(car2, T, T + 2 * H, true);
  add_readings(car2, T, T + 2 * H);
  TEST_ASSERT_EQUAL_INT(0, api_resolve_km(a, T + 3 * H));
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->km_pending);
}

static void test_resolve_skips_manual(void) {
  int64_t id = add_pending(car1, T, T + 2 * H, false);
  add_readings(car1, T, T + 2 * H);
  TEST_ASSERT_EQUAL_INT(0, api_resolve_km(a, T + 3 * H));
  const booking_t *b = db_booking(&a->db, id);
  TEST_ASSERT_EQUAL_INT(KM_NONE, b->km_source);
  TEST_ASSERT_EQUAL_INT64(0, b->km_end);
}

static void test_mail_after_24h_to_active_admins_only(void) {
  int64_t now = T + 10 * 86400;
  int64_t id = add_pending(car1, now - 90000, now - 86401, true);
  TEST_ASSERT_EQUAL_INT(1, api_km_pending_mail(a, now));
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("admin@x.de", th_last_mail_to());
  TEST_ASSERT_EQUAL_STRING("km ausstehend", th_last_mail_subject());
  char want[64];
  snprintf(want, sizeof want, "Buchung %lld: Golf", (long long)id);
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), want));
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "/#/admin/buchungen"));
  TEST_ASSERT_EQUAL_INT64(now / 86400, a->db.km_mail_day);
}

static void test_mail_not_before_24h(void) {
  int64_t now = T + 10 * 86400;
  add_pending(car1, now - 7200, now - 3600, true);
  TEST_ASSERT_EQUAL_INT(0, api_km_pending_mail(a, now));
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
}

static void test_mail_once_per_day_survives_restart(void) {
  int64_t now = T + 10 * 86400;
  add_pending(car1, now - 90000, now - 86401, true);
  TEST_ASSERT_EQUAL_INT(1, api_km_pending_mail(a, now));
  char dir[256];
  snprintf(dir, sizeof dir, "%s", a->cfg.data_dir);
  db_free(&a->db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&a->db, dir));
  TEST_ASSERT_EQUAL_INT(0, api_km_pending_mail(a, now));
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
  TEST_ASSERT_EQUAL_INT(1, api_km_pending_mail(a, now + 86400));
  TEST_ASSERT_EQUAL_INT(2, th_mail_count());
}

static void test_mail_ignores_car_without_tracker(void) {
  int64_t now = T + 10 * 86400;
  add_pending(car2, now - 90000, now - 86401, true);
  TEST_ASSERT_EQUAL_INT(0, api_km_pending_mail(a, now));
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
}

static void test_mail_all_fail_retries(void) {
  int64_t now = T + 10 * 86400;
  add_pending(car1, now - 90000, now - 86401, true);
  th_mail_fail(1);
  TEST_ASSERT_EQUAL_INT(-1, api_km_pending_mail(a, now));
  TEST_ASSERT_EQUAL_INT64(0, a->db.km_mail_day);
  th_mail_fail(0);
  TEST_ASSERT_EQUAL_INT(1, api_km_pending_mail(a, now));
  TEST_ASSERT_EQUAL_INT64(now / 86400, a->db.km_mail_day);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_resolve_sets_telematics_km);
  RUN_TEST(test_resolve_waits_without_later_reading);
  RUN_TEST(test_resolve_skips_car_without_tracker);
  RUN_TEST(test_resolve_skips_manual);
  RUN_TEST(test_mail_after_24h_to_active_admins_only);
  RUN_TEST(test_mail_not_before_24h);
  RUN_TEST(test_mail_once_per_day_survives_restart);
  RUN_TEST(test_mail_ignores_car_without_tracker);
  RUN_TEST(test_mail_all_fail_retries);
  return UNITY_END();
}
