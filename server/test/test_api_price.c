#include <stdio.h>
#include <string.h>

#include "auth/session.h"
#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400
#define H 3600
#define DAY 86400
/* 2026-03-28 14:00 Europe/Berlin (CET): a plain day-rate afternoon. */
#define B0 (T - 11 * H)

static app_t *a;
static int64_t adm, u1, car1, car2;
static char tadm[65], tu1[65];

static int64_t add_car(const char *name) {
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "%s", name);
  snprintf(c.plate, sizeof c.plate, "X-%s", name);
  snprintf(c.location, sizeof c.location, "Hof");
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  return c.id;
}

static void add_tariff(int64_t car, int64_t valid_from) {
  tariff_t t;
  memset(&t, 0, sizeof t);
  t.car_id = car;
  t.valid_from = valid_from;
  t.rate_day_cents_per_h = 300;
  t.rate_night_cents_per_h = 150;
  t.rate_km_cents = 30;
  t.night_start = 1200;
  t.night_end = 360;
  t.unused_factor_permille = 500;
  t.time_cap_cents_per_day = 3000;
  t.combined_cap_cents_per_day = 5000;
  t.late_fee_cents_per_started_h = 1500;
  t.created_at = T;
  TEST_ASSERT_EQUAL_INT(0, db_put_tariff(&a->db, &t));
}

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  adm = th_add_user(a, "admin@x.de", NULL, ROLE_ADMIN);
  u1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(adm > 0 && u1 > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tadm));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, T, tu1));
  car1 = add_car("Golf");
  car2 = add_car("Polo");
  add_tariff(car1, T - 100 * DAY);
}

void tearDown(void) { th_free(a); }

static cJSON *call(const char *p, const char *tok, int status, const char *code) {
  api_res_t r = th_req(a, "GET", p, NULL, tok, T);
  TEST_ASSERT_NOT_NULL(r.body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(status, r.status, r.body);
  cJSON *j = cJSON_Parse(r.body);
  api_res_free(&r);
  TEST_ASSERT_NOT_NULL(j);
  if (code) {
    cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
    if (!cJSON_IsString(e)) cJSON_Delete(j);
    TEST_ASSERT_TRUE(cJSON_IsString(e));
    TEST_ASSERT_EQUAL_STRING(code, e->valuestring);
  }
  return j;
}

static int64_t num(const cJSON *o, const char *k) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsNumber(v));
  return (int64_t)v->valuedouble;
}

static void check_price(const cJSON *item, const char *status, int64_t total) {
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "price");
  TEST_ASSERT_TRUE(cJSON_IsObject(p));
  const cJSON *s = cJSON_GetObjectItemCaseSensitive(p, "status");
  TEST_ASSERT_TRUE(cJSON_IsString(s));
  TEST_ASSERT_EQUAL_STRING(status, s->valuestring);
  TEST_ASSERT_EQUAL_INT64(total, num(p, "total_cents"));
}

static int64_t put_done(int64_t car, int64_t start, int64_t end, int64_t km0, int64_t km1) {
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = car;
  b.user_id = u1;
  b.booked_start = start;
  b.booked_end = end;
  b.state = BK_DONE;
  b.actual_start = start;
  b.actual_end = end;
  b.km_start = km0;
  b.km_end = km1;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  return b.id;
}

void test_history_has_price(void) {
  put_done(car1, B0, B0 + 4 * H, 1000, 1040);
  cJSON *j = call("/api/bookings?scope=history", tu1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  check_price(cJSON_GetArrayItem(j, 0), "ok", 2400);
  cJSON_Delete(j);
}

void test_admin_bookings_have_price(void) {
  put_done(car1, B0, B0 + 4 * H, 1000, 1040);
  cJSON *j = call("/api/admin/bookings", tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  check_price(cJSON_GetArrayItem(j, 0), "ok", 2400);
  cJSON_Delete(j);
}

void test_price_km_pending(void) {
  int64_t id = put_done(car1, B0, B0 + 4 * H, 0, 0);
  db_booking(&a->db, id)->km_pending = true;
  cJSON *j = call("/api/bookings?scope=history", tu1, 200, NULL);
  check_price(cJSON_GetArrayItem(j, 0), "km_pending", 1200);
  cJSON_Delete(j);
}

void test_price_no_tariff(void) {
  put_done(car2, B0, B0 + 4 * H, 1000, 1040);
  add_tariff(car2, B0 + DAY);
  cJSON *j = call("/api/bookings?scope=history", tu1, 200, NULL);
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(j, 0), "price");
  TEST_ASSERT_EQUAL_STRING("no_tariff", cJSON_GetObjectItemCaseSensitive(p, "status")->valuestring);
  TEST_ASSERT_EQUAL_INT64(0, num(p, "total_cents"));
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(p, "tariff")));
  cJSON_Delete(j);
}

void test_cancelled_price_is_fee(void) {
  int64_t id = put_done(car1, B0, B0 + 4 * H, 0, 0);
  booking_t *b = db_booking(&a->db, id);
  b->state = BK_CANCELLED;
  b->actual_start = b->actual_end = 0;
  b->cancel_fee_cents = 500;
  cJSON *j = call("/api/bookings?scope=history", tu1, 200, NULL);
  check_price(cJSON_GetArrayItem(j, 0), "ok", 500);
  cJSON_Delete(j);
}

static char *est(int64_t car, int64_t s, int64_t e, const char *rest) {
  static char p[200];
  snprintf(p, sizeof p, "/api/price-estimate?car=%lld&start=%lld&end=%lld%s", (long long)car,
           (long long)s, (long long)e, rest);
  return p;
}

void test_estimate_basic(void) {
  cJSON *j = call(est(car1, B0, B0 + 4 * H, "&km=15"), tu1, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_EQUAL_INT64(1650, num(j, "total_cents"));
  TEST_ASSERT_EQUAL_INT64(300, num(cJSON_GetObjectItemCaseSensitive(j, "tariff"), "rate_day_cents_per_h"));
  cJSON_Delete(j);
}

void test_estimate_club(void) {
  cJSON *j = call(est(car1, B0, B0 + 4 * H, "&km=15&club=1"), tu1, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_EQUAL_INT64(0, num(j, "total_cents"));
  cJSON_Delete(j);
}

void test_estimate_rejects_bad_input(void) {
  cJSON_Delete(call(est(car1, B0, B0, "&km=1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(car1, B0, B0 - H, "&km=1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(car1, B0, B0 + H, "&km=-1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(car1, B0, B0 + H, "&km=10000001"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(car1, B0, B0 + H, "&km=10000000"), tu1, 200, NULL));
  cJSON_Delete(call(est(car1, B0, B0 + H, ""), tu1, 400, "invalid"));
  cJSON_Delete(call(est(car1, B0, B0 + H, "&km=5x"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(car1, B0, B0 + H, "&km=99999999999999999999"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(car1, B0, B0 + H, "&km=1&club=2"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(car1, B0, B0 + 90 * DAY + 1, "&km=1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(car1, B0, B0 + 90 * DAY, "&km=1"), tu1, 200, NULL));
  cJSON_Delete(call(est(999, B0, B0 + H, "&km=1"), tu1, 404, "not_found"));
  car_t *c = db_car(&a->db, car2);
  c->active = false;
  cJSON_Delete(call(est(car2, B0, B0 + H, "&km=1"), tu1, 404, "not_found"));
  c->active = true;
  cJSON *j = call(est(car2, B0, B0 + H, "&km=1"), tu1, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("no_tariff", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "tariff")));
  cJSON_Delete(j);
}

void test_estimate_requires_login(void) {
  cJSON_Delete(call(est(car1, B0, B0 + H, "&km=1"), NULL, 401, NULL));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_history_has_price);
  RUN_TEST(test_admin_bookings_have_price);
  RUN_TEST(test_price_km_pending);
  RUN_TEST(test_price_no_tariff);
  RUN_TEST(test_cancelled_price_is_fee);
  RUN_TEST(test_estimate_basic);
  RUN_TEST(test_estimate_club);
  RUN_TEST(test_estimate_rejects_bad_input);
  RUN_TEST(test_estimate_requires_login);
  return UNITY_END();
}
