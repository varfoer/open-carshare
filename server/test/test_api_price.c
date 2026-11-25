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
static int64_t adm, u1, ured, car1, car2, std, red;
static char tadm[65], tu1[65], tured[65];

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
  std = th_add_tariff(a, "Standard");
  TEST_ASSERT_TRUE(std > 0);
  TEST_ASSERT_TRUE(th_add_version(a, std, T - 100 * DAY, 3000, 5000, 1500) > 0);
  TEST_ASSERT_TRUE(th_assign_tariff(a, u1, std, T - 100 * DAY) > 0);
  /* "Reduziert": standard rates except 1,50 €/h by day, no caps */
  red = th_add_tariff(a, "Reduziert");
  TEST_ASSERT_TRUE(red > 0);
  int64_t vid = th_add_version(a, red, T - 100 * DAY, 0, 0, 0);
  TEST_ASSERT_TRUE(vid > 0);
  tariff_version_t v = *db_version(&a->db, vid);
  v.rate_day_cents_per_h = 150;
  TEST_ASSERT_EQUAL_INT(0, db_put_version(&a->db, &v));
  ured = th_add_user(a, "ute@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(ured > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, ured, T, tured));
  TEST_ASSERT_TRUE(th_assign_tariff(a, ured, red, T - 100 * DAY) > 0);
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
  b.km = km1 - km0;
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
  /* adm's tariff only gets a version after the booking */
  int64_t late = th_add_tariff(a, "Sp\xC3\xA4t");
  TEST_ASSERT_TRUE(late > 0);
  TEST_ASSERT_TRUE(th_add_version(a, late, B0 + DAY, 3000, 5000, 1500) > 0);
  TEST_ASSERT_TRUE(th_assign_tariff(a, adm, late, T - 100 * DAY) > 0);
  int64_t id = put_done(car2, B0, B0 + 4 * H, 1000, 1040);
  db_booking(&a->db, id)->user_id = adm;
  cJSON *j = call("/api/bookings?scope=history", tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
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

static char *est(int64_t s, int64_t e, const char *rest) {
  static char p[200];
  snprintf(p, sizeof p, "/api/price-estimate?start=%lld&end=%lld%s", (long long)s, (long long)e,
           rest);
  return p;
}

void test_estimate_basic(void) {
  cJSON *j = call(est(B0, B0 + 4 * H, "&km=15"), tu1, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_EQUAL_INT64(1650, num(j, "total_cents"));
  TEST_ASSERT_EQUAL_INT64(300, num(cJSON_GetObjectItemCaseSensitive(j, "tariff"), "rate_day_cents_per_h"));
  cJSON_Delete(j);
}

void test_estimate_club(void) {
  cJSON *j = call(est(B0, B0 + 4 * H, "&km=15&club=1"), tu1, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_EQUAL_INT64(0, num(j, "total_cents"));
  cJSON_Delete(j);
}

void test_estimate_rejects_bad_input(void) {
  cJSON_Delete(call(est(B0, B0, "&km=1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, B0 - H, "&km=1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, B0 + H, "&km=-1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, B0 + H, "&km=10000001"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, B0 + H, "&km=10000000"), tu1, 200, NULL));
  cJSON_Delete(call(est(B0, B0 + H, ""), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, B0 + H, "&km=5x"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, B0 + H, "&km=99999999999999999999"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, B0 + H, "&km=1&club=2"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, B0 + 90 * DAY + 1, "&km=1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, B0 + 90 * DAY, "&km=1"), tu1, 200, NULL));
  /* times bounded like booking writes: no huge/zero start (day loop, overflow) */
  cJSON_Delete(call(est(T + 2 * 365 * DAY + 1, T + 2 * 365 * DAY + H, "&km=1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(B0, T + 2 * 365 * DAY + 1, "&km=1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(INT64_MAX - H, INT64_MAX, "&km=1"), tu1, 400, "invalid"));
  cJSON_Delete(call(est(0, H, "&km=1"), tu1, 400, "invalid"));
  /* a user without tariff assignment gets no_tariff, for estimates and bookings alike */
  char tu2[65];
  int64_t u2 = th_add_user(a, "bo@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(u2 > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u2, T, tu2));
  cJSON *j = call(est(B0, B0 + H, "&km=1"), tu2, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("no_tariff", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "tariff")));
  cJSON_Delete(j);
  int64_t id = put_done(car1, B0, B0 + 4 * H, 1000, 1040);
  db_booking(&a->db, id)->user_id = u2;
  j = call("/api/bookings?scope=history", tu2, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(j, 0), "price");
  TEST_ASSERT_EQUAL_STRING("no_tariff", cJSON_GetObjectItemCaseSensitive(p, "status")->valuestring);
  cJSON_Delete(j);
}

void test_booking_create_has_price(void) {
  char body[160];
  snprintf(body, sizeof body, "{\"car_id\":%lld,\"start\":%lld,\"end\":%lld}", (long long)car1,
           (long long)(T + DAY), (long long)(T + DAY + H));
  api_res_t r = th_req(a, "POST", "/api/bookings", body, tu1, T);
  TEST_ASSERT_NOT_NULL(r.body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(201, r.status, r.body);
  cJSON *j = cJSON_Parse(r.body);
  api_res_free(&r);
  TEST_ASSERT_NOT_NULL(j);
  TEST_ASSERT_TRUE(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(j, "price")));
  cJSON_Delete(j);
}

void test_estimate_requires_login(void) {
  cJSON_Delete(call(est(B0, B0 + H, "&km=1"), NULL, 401, NULL));
}

void test_estimate_per_user(void) {
  cJSON *j = call(est(B0, B0 + 4 * H, "&km=15"), tu1, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_EQUAL_INT64(1650, num(j, "total_cents"));
  cJSON_Delete(j);
  j = call(est(B0, B0 + 4 * H, "&km=15"), tured, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_EQUAL_INT64(1050, num(j, "total_cents")); /* 4 h x 1,50 EUR + 15 km x 0,30 EUR */
  TEST_ASSERT_EQUAL_INT64(red, num(cJSON_GetObjectItemCaseSensitive(j, "tariff"), "tariff_id"));
  cJSON_Delete(j);
}

void test_estimate_club_zero_any_tariff(void) {
  cJSON *j = call(est(B0, B0 + 4 * H, "&km=15&club=1"), tured, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_EQUAL_INT64(0, num(j, "total_cents"));
  cJSON_Delete(j);
  char tu3[65];
  int64_t u3 = th_add_user(a, "cara@x.de", NULL, ROLE_USER); /* no tariff assigned */
  TEST_ASSERT_TRUE(u3 > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u3, T, tu3));
  j = call(est(B0, B0 + 4 * H, "&km=15&club=1"), tu3, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("ok", cJSON_GetObjectItemCaseSensitive(j, "status")->valuestring);
  TEST_ASSERT_EQUAL_INT64(0, num(j, "total_cents"));
  cJSON_Delete(j);
}

static void check_tariff_keys(const cJSON *t) {
  TEST_ASSERT_TRUE(cJSON_IsObject(t));
  TEST_ASSERT_EQUAL_INT64(300, num(t, "rate_day_cents_per_h"));
  TEST_ASSERT_EQUAL_INT64(std, num(t, "tariff_id"));
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(t, "name"));
}

void test_price_tariff_has_no_name(void) {
  cJSON *j = call(est(B0, B0 + 4 * H, "&km=15"), tu1, 200, NULL);
  check_tariff_keys(cJSON_GetObjectItemCaseSensitive(j, "tariff"));
  cJSON_Delete(j);
  put_done(car1, B0, B0 + 4 * H, 1000, 1040);
  j = call("/api/bookings?scope=history", tu1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(j, 0), "price");
  check_tariff_keys(cJSON_GetObjectItemCaseSensitive(p, "tariff"));
  cJSON_Delete(j);
}

void test_switch_applies_from_valid_from(void) {
  TEST_ASSERT_TRUE(th_assign_tariff(a, u1, red, B0 + 2 * H) > 0);
  int64_t before = put_done(car1, B0, B0 + H, 0, 0);
  int64_t after = put_done(car2, B0 + 2 * H, B0 + 3 * H, 0, 0);
  cJSON *j = call("/api/bookings?scope=history", tu1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  int seen = 0;
  const cJSON *it;
  cJSON_ArrayForEach(it, j) {
    int64_t id = num(it, "id");
    if (id == before) check_price(it, "ok", 300);
    else if (id == after) check_price(it, "ok", 150);
    else TEST_FAIL_MESSAGE("unexpected booking");
    seen++;
  }
  TEST_ASSERT_EQUAL_INT(2, seen);
  cJSON_Delete(j);
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
  RUN_TEST(test_booking_create_has_price);
  RUN_TEST(test_estimate_requires_login);
  RUN_TEST(test_estimate_per_user);
  RUN_TEST(test_estimate_club_zero_any_tariff);
  RUN_TEST(test_price_tariff_has_no_name);
  RUN_TEST(test_switch_applies_from_valid_from);
  return UNITY_END();
}
