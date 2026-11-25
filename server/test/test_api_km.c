#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "auth/session.h"
#include "core/pricing.h"
#include "helpers.h"
#include "http/api.h"
#include "http/api_internal.h"
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
  TEST_ASSERT_EQUAL_INT64(12, b->km);
  TEST_ASSERT_EQUAL_INT(KM_TELEMATICS, b->km_source);
  TEST_ASSERT_FALSE(b->km_pending);
  TEST_ASSERT_EQUAL_INT64(T + 3 * H, b->modified_at);
  TEST_ASSERT_EQUAL_INT64(0, b->modified_by);
  char dir[256];
  snprintf(dir, sizeof dir, "%s", a->cfg.data_dir);
  db_free(&a->db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&a->db, dir));
  b = db_booking(&a->db, id);
  TEST_ASSERT_EQUAL_INT64(12, b->km);
  TEST_ASSERT_EQUAL_INT(KM_TELEMATICS, b->km_source);
  TEST_ASSERT_FALSE(b->km_pending);
}

/* Newest-first sending: the parked record arrives before the trip-end backlog. */
static void test_resolve_waits_for_late_trip_end_records(void) {
  int64_t end = T + 2 * H;
  int64_t id = add_pending(car1, T, end, true);
  odometer_reading_t first[2] = {{car1, T + 60, 1000000, T + 3 * H}, {car1, end + 100, 1012000, T + 3 * H}};
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&a->db, first, 2));
  TEST_ASSERT_EQUAL_INT(0, api_resolve_km(a, T + 3 * H + 599));
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->km_pending);
  odometer_reading_t late[1] = {{car1, end - 60, 1030000, T + 3 * H + 300}};
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&a->db, late, 1));
  TEST_ASSERT_EQUAL_INT(0, api_resolve_km(a, T + 3 * H + 599));
  TEST_ASSERT_EQUAL_INT(1, api_resolve_km(a, T + 3 * H + 600));
  const booking_t *b = db_booking(&a->db, id);
  TEST_ASSERT_EQUAL_INT64(30, b->km);
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
  TEST_ASSERT_EQUAL_INT64(0, b->km);
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

/* ---- live km and price ---- */

#define B0 (T + 8 * H) /* 10:00 CEST */
#define B1 (T + 12 * H)

static int64_t add_trip(int64_t car, booking_state_t st, int64_t a0, int64_t a1, bool pending) {
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = car;
  b.user_id = user1;
  b.booked_start = B0;
  b.booked_end = B1;
  b.state = st;
  b.actual_start = a0;
  b.actual_end = a1;
  b.km_pending = pending;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  return b.id;
}

static void reading(int64_t car, int64_t ts, int64_t m) {
  odometer_reading_t r = {car, ts, m, ts};
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&a->db, &r, 1));
}

static const tariff_version_t *standard_tariff(void) {
  int64_t t = th_add_tariff(a, "Standard");
  TEST_ASSERT_TRUE(th_add_version(a, t, T - 86400, 0, 0, 500) > 0);
  TEST_ASSERT_TRUE(th_assign_tariff(a, user1, t, T - 86400) > 0);
  return db_tariff_at(&a->db, user1, B0);
}

static cJSON *live_of(int64_t id, int64_t now) {
  cJSON *o = api_booking_json(a, db_booking(&a->db, id), now);
  TEST_ASSERT_NOT_NULL(o);
  return o;
}

static int64_t num(const cJSON *o, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsNumber(v));
  return (int64_t)v->valuedouble;
}

static void test_live_active_return_now(void) {
  const tariff_version_t *t = standard_tariff();
  int64_t id = add_trip(car1, BK_ACTIVE, B0, 0, false);
  reading(car1, B0 - 600, 999000);      /* before the trip: not counted */
  reading(car1, B0 + 60, 1000000);
  reading(car1, B0 + H - 60, 1023400);  /* 23.4 km */
  cJSON *o = live_of(id, B0 + H);
  const cJSON *live = cJSON_GetObjectItemCaseSensitive(o, "live");
  TEST_ASSERT_TRUE(cJSON_IsObject(live));
  TEST_ASSERT_EQUAL_INT64(23, num(live, "km"));
  TEST_ASSERT_EQUAL_INT64(B0 + H - 60, num(live, "as_of"));
  price_t p;
  price_compute(t, a->db.settings.start_tolerance_min, B0, B1, B0, B0 + H, 23, &p);
  const cJSON *lp = cJSON_GetObjectItemCaseSensitive(live, "price");
  TEST_ASSERT_EQUAL_INT64(p.total_cents, num(lp, "total_cents"));
  TEST_ASSERT_EQUAL_INT64(23, num(lp, "km"));
  TEST_ASSERT_TRUE(num(lp, "unused_day_min") > 0); /* rest of the booked time counted as unused */
  TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetObjectItemCaseSensitive(lp, "status")->valuestring);
  /* the booking's own price is unchanged: booked time, km unknown */
  TEST_ASSERT_EQUAL_STRING("km_pending",
                           cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(o, "price"), "status")->valuestring);
  cJSON_Delete(o);
  /* past the booked end: late fee included */
  o = live_of(id, B1 + 30 * 60);
  TEST_ASSERT_TRUE(num(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(o, "live"), "price"),
                       "late_fee_cents") > 0);
  cJSON_Delete(o);
}

static void test_live_after_return_until_km(void) {
  standard_tariff();
  int64_t id = add_trip(car1, BK_DONE, B0, B0 + 2 * H, true);
  reading(car1, B0 + 60, 1000000);
  reading(car1, B0 + 2 * H - 300, 1041000);
  reading(car1, B0 + 2 * H + 600, 1045000); /* after the return: not counted */
  cJSON *o = live_of(id, B0 + 3 * H);
  const cJSON *live = cJSON_GetObjectItemCaseSensitive(o, "live");
  TEST_ASSERT_EQUAL_INT64(41, num(live, "km"));
  TEST_ASSERT_EQUAL_INT64(B0 + 2 * H - 300, num(live, "as_of"));
  cJSON_Delete(o);
  /* resolved: no live part any more */
  booking_t b = *db_booking(&a->db, id);
  b.km_pending = false;
  b.km_source = KM_TELEMATICS;
  b.km = 41;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  o = live_of(id, B0 + 3 * H);
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(o, "live")));
  cJSON_Delete(o);
}

static void test_live_null_cases(void) {
  int64_t no_tracker = add_trip(car2, BK_ACTIVE, B0, 0, false);
  reading(car2, B0 + 60, 1000000);
  int64_t no_reading = add_trip(car1, BK_ACTIVE, B0, 0, false);
  int64_t reserved = add_trip(car1, BK_RESERVED, 0, 0, false);
  int64_t ids[] = {no_tracker, no_reading, reserved};
  for (size_t i = 0; i < 3; i++) {
    cJSON *o = live_of(ids[i], B0 + H);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(o, "live")));
    cJSON_Delete(o);
  }
}

static void test_live_in_bookings_list(void) {
  standard_tariff();
  add_trip(car1, BK_ACTIVE, B0, 0, false);
  reading(car1, B0 + 60, 1000000);
  reading(car1, B0 + 600, 1005000);
  char tok[65];
  TEST_ASSERT_EQUAL_INT(0, session_create(a, user1, B0, tok));
  api_res_t r = th_req(a, "GET", "/api/bookings", NULL, tok, B0 + 700);
  TEST_ASSERT_EQUAL_INT(200, r.status);
  cJSON *arr = cJSON_Parse(r.body);
  api_res_free(&r);
  const cJSON *live = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 0), "live");
  TEST_ASSERT_EQUAL_INT64(5, num(live, "km"));
  cJSON_Delete(arr);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_resolve_sets_telematics_km);
  RUN_TEST(test_resolve_waits_without_later_reading);
  RUN_TEST(test_resolve_waits_for_late_trip_end_records);
  RUN_TEST(test_resolve_skips_car_without_tracker);
  RUN_TEST(test_resolve_skips_manual);
  RUN_TEST(test_mail_after_24h_to_active_admins_only);
  RUN_TEST(test_mail_not_before_24h);
  RUN_TEST(test_mail_once_per_day_survives_restart);
  RUN_TEST(test_mail_ignores_car_without_tracker);
  RUN_TEST(test_mail_all_fail_retries);
  RUN_TEST(test_live_active_return_now);
  RUN_TEST(test_live_after_return_until_km);
  RUN_TEST(test_live_null_cases);
  RUN_TEST(test_live_in_bookings_list);
  return UNITY_END();
}
