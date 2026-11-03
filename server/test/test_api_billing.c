#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/session.h"
#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define H 3600
/* 2026-06-10 12:00 UTC: April and May 2026 are closed, June is open. */
#define N 1781092800
/* 10:00 UTC = 12:00 Berlin summer time, day rate. */
#define MAR20 1774000800
#define APR10 1775815200
#define APR12 1775988000
#define APR20 1776679200
#define MAY2 1777716000
#define MAY12 1778580000
#define FULL_IBAN "DE89370400440532013000"

static app_t *a;
static int64_t adm, uz, ua, car1, car2;   /* uz "Zora" has the lower id, ua "Anna" sorts first */
static char tadm[65];

static void set_name(int64_t id, const char *name) {
  user_t u = *db_user(&a->db, id);
  snprintf(u.name, sizeof u.name, "%s", name);
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
}

static int64_t add_car(const char *name, bool tariff) {
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "%s", name);
  snprintf(c.plate, sizeof c.plate, "X-%s", name);
  snprintf(c.location, sizeof c.location, "Hof");
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  if (tariff) {
    tariff_t t;
    memset(&t, 0, sizeof t);
    t.car_id = c.id;
    t.valid_from = 1;
    t.rate_day_cents_per_h = 300;
    t.rate_night_cents_per_h = 150;
    t.rate_km_cents = 30;
    t.night_start = 1200;
    t.night_end = 360;
    t.unused_factor_permille = 500;
    t.late_fee_cents_per_started_h = 1500;
    t.created_at = 1;
    TEST_ASSERT_EQUAL_INT(0, db_put_tariff(&a->db, &t));
  }
  return c.id;
}

/* DONE booking of h hours driving km kilometres, used exactly as booked. */
static int64_t add_done(int64_t user, int64_t car, int64_t start, int h, int km) {
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = car;
  b.user_id = user;
  b.booked_start = b.actual_start = start;
  b.booked_end = b.actual_end = start + h * H;
  b.state = BK_DONE;
  b.km_start = 100;
  b.km_end = 100 + km;
  b.km_source = KM_MANUAL;
  b.created_at = b.modified_at = start - H;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  return b.id;
}

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  adm = th_add_user(a, "admin@x.de", NULL, ROLE_ADMIN);
  uz = th_add_user(a, "zora@x.de", NULL, ROLE_USER);
  ua = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(adm > 0 && uz > 0 && ua > 0);
  set_name(uz, "Zora Z");
  set_name(ua, "Anna A");
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, N, tadm));
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  settings_t s = a->db.settings;
  snprintf(s.creditor_iban, sizeof s.creditor_iban, "%s", FULL_IBAN);
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&a->db, &s));
  car1 = add_car("Golf", true);
  car2 = add_car("Polo", false);
}

void tearDown(void) { th_free(a); }

static cJSON *call(const char *m, const char *p, const char *body, int status, const char *code) {
  api_res_t r = th_req(a, m, p, body, tadm, N);
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

static void call_free(const char *m, const char *p, const char *body, int status, const char *code) {
  cJSON_Delete(call(m, p, body, status, code));
}

static int64_t num(const cJSON *o, const char *k) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsNumber(v));
  return (int64_t)v->valuedouble;
}

static cJSON *obj(const cJSON *o, const char *k) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_NOT_NULL(v);
  return v;
}

static int64_t create_run(const char *period) {
  char p[64];
  snprintf(p, sizeof p, "/api/admin/billing?period=%s", period);
  cJSON *j = call("POST", p, NULL, 201, NULL);
  int64_t id = num(j, "id");
  TEST_ASSERT_EQUAL_STRING("draft", obj(j, "state")->valuestring);
  TEST_ASSERT_EQUAL_STRING(period, obj(j, "period")->valuestring);
  cJSON_Delete(j);
  return id;
}

static const char *rpath(int64_t id, const char *suffix) {
  static char p[96];
  snprintf(p, sizeof p, "/api/admin/billing/%lld%s", (long long)id, suffix);
  return p;
}

static cJSON *detail(int64_t id) { return call("GET", rpath(id, ""), NULL, 200, NULL); }

/* Standard April data: Anna two trips (900 + 300), Zora one (600). */
static void add_april(void) {
  add_done(ua, car1, APR12, 1, 0);
  add_done(ua, car1, APR10, 2, 10);
  add_done(uz, car1, APR20, 2, 0);
}

void test_create_requires_closed_month(void) {
  call_free("POST", "/api/admin/billing?period=2026-06", NULL, 400, "period_open");
  call_free("POST", "/api/admin/billing?period=2026-13", NULL, 400, "invalid");
  call_free("POST", "/api/admin/billing", NULL, 400, "invalid");
  call_free("POST", "/api/admin/billing?period=1999-01", NULL, 400, "invalid");
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_runs);
  create_run("2026-05");
  TEST_ASSERT_EQUAL_UINT(1, a->db.n_runs);
}

void test_create_draft_exists_409(void) {
  create_run("2026-04");
  call_free("POST", "/api/admin/billing?period=2026-05", NULL, 409, "draft_exists");
}

void test_create_not_ascending_409(void) {
  add_april();
  int64_t id = create_run("2026-04");
  call_free("POST", rpath(id, "/finalize"), NULL, 200, NULL);
  call_free("POST", "/api/admin/billing?period=2026-04", NULL, 409, "period_exists");
  call_free("POST", "/api/admin/billing?period=2026-03", NULL, 409, "period_exists");
  create_run("2026-05");
}

void test_draft_lists_users_lines_totals(void) {
  add_april();
  int64_t id = create_run("2026-04");
  cJSON *j = detail(id);
  TEST_ASSERT_EQUAL_INT64(1800, num(j, "total_cents"));
  cJSON *users = obj(j, "users");
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(users));
  cJSON *u0 = cJSON_GetArrayItem(users, 0), *u1 = cJSON_GetArrayItem(users, 1);
  TEST_ASSERT_EQUAL_INT64(ua, num(u0, "user_id"));
  TEST_ASSERT_EQUAL_STRING("Anna A", obj(u0, "name")->valuestring);
  TEST_ASSERT_EQUAL_STRING("anna@x.de", obj(u0, "email")->valuestring);
  TEST_ASSERT_EQUAL_INT64(1200, num(u0, "total_cents"));
  TEST_ASSERT_EQUAL_INT64(uz, num(u1, "user_id"));
  TEST_ASSERT_EQUAL_INT64(600, num(u1, "total_cents"));
  cJSON *lines = obj(u0, "lines");
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(lines));
  TEST_ASSERT_EQUAL_INT64(APR10, num(cJSON_GetArrayItem(lines, 0), "start"));
  TEST_ASSERT_EQUAL_INT64(900, num(cJSON_GetArrayItem(lines, 0), "total_cents"));
  TEST_ASSERT_EQUAL_INT64(10, num(cJSON_GetArrayItem(lines, 0), "km"));
  TEST_ASSERT_EQUAL_INT64(APR12, num(cJSON_GetArrayItem(lines, 1), "start"));
  TEST_ASSERT_EQUAL_INT64(300, num(cJSON_GetArrayItem(lines, 1), "total_cents"));
  TEST_ASSERT_TRUE(cJSON_IsNull(obj(u0, "invoice")));
  cJSON *m = obj(u0, "mandate");
  TEST_ASSERT_TRUE(strncmp(obj(m, "ref")->valuestring, "OCS-", 4) == 0);
  TEST_ASSERT_NOT_NULL(strstr(obj(m, "iban_masked")->valuestring, "****"));
  char *raw = cJSON_PrintUnformatted(j);
  TEST_ASSERT_NULL(strstr(raw, FULL_IBAN));
  free(raw);
  cJSON *bl = obj(j, "blockers");
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(obj(bl, "km_pending")));
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(obj(bl, "no_tariff")));
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(obj(bl, "no_mandate")));
  cJSON_Delete(j);
  j = call("GET", "/api/admin/billing", NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT64(0, num(cJSON_GetArrayItem(j, 0), "n_invoices"));
  cJSON_Delete(j);
}

void test_draft_zero_invoice_club_only(void) {
  int64_t ub = th_add_user_bare(a, "club@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(ub > 0);
  set_name(ub, "Club C");
  int64_t bid = add_done(ub, car1, APR10, 2, 0);
  booking_t b = *db_booking(&a->db, bid);
  b.club_trip = true;
  b.km_start = b.km_end = 0;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  int64_t id = create_run("2026-04");
  cJSON *j = detail(id);
  cJSON *users = obj(j, "users");
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(users));
  cJSON *u = cJSON_GetArrayItem(users, 0);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(obj(u, "lines")));
  TEST_ASSERT_EQUAL_INT64(0, num(u, "total_cents"));
  TEST_ASSERT_TRUE(cJSON_IsNull(obj(u, "mandate")));
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(obj(obj(j, "blockers"), "no_mandate")));
  cJSON_Delete(j);
  j = call("POST", rpath(id, "/finalize"), NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(0, num(j, "total_cents"));
  cJSON_Delete(j);
  const invoice_t *inv = db_invoice(&a->db, 1);
  TEST_ASSERT_NOT_NULL(inv);
  TEST_ASSERT_EQUAL_INT64(0, inv->total_cents);
  TEST_ASSERT_EQUAL_INT64(1, inv->n_lines);
}

void test_draft_no_line_no_user(void) {
  add_done(ua, car1, APR10, 2, 0);
  add_done(uz, car1, MAY2, 2, 0);                 /* after the period */
  booking_t b;                                     /* cancelled without fee */
  memset(&b, 0, sizeof b);
  b.car_id = car1;
  b.user_id = uz;
  b.booked_start = APR20;
  b.booked_end = APR20 + H;
  b.state = BK_CANCELLED;
  b.cancelled_at = APR10;
  b.cancel_fee_cents = 0;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  cJSON *j = detail(create_run("2026-04"));
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(obj(j, "users")));
  TEST_ASSERT_EQUAL_INT64(ua, num(cJSON_GetArrayItem(obj(j, "users"), 0), "user_id"));
  cJSON_Delete(j);
}

void test_draft_straggler_included(void) {
  int64_t bid = add_done(ua, car1, MAR20, 2, 0);
  cJSON *j = detail(create_run("2026-04"));
  cJSON *lines = obj(cJSON_GetArrayItem(obj(j, "users"), 0), "lines");
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(lines));
  TEST_ASSERT_EQUAL_INT64(bid, num(cJSON_GetArrayItem(lines, 0), "booking_id"));
  cJSON_Delete(j);
}

static void expect_blocker(const char *name, int64_t id) {
  cJSON *j = call("POST", rpath(a->db.runs[0].id, "/finalize"), NULL, 409, "blocked");
  cJSON *bl = obj(j, "blockers");
  const char *keys[3] = {"km_pending", "no_tariff", "no_mandate"};
  for (int i = 0; i < 3; i++) {
    cJSON *arr = obj(bl, keys[i]);
    if (strcmp(keys[i], name) == 0) {
      TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(arr));
      TEST_ASSERT_EQUAL_INT64(id, (int64_t)cJSON_GetArrayItem(arr, 0)->valuedouble);
    } else {
      TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(arr));
    }
  }
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(RUN_DRAFT, a->db.runs[0].state);
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_invoices);
}

void test_finalize_blocked_km_pending(void) {
  int64_t bid = add_done(ua, car1, APR10, 2, 0);
  booking_t b = *db_booking(&a->db, bid);
  b.km_pending = true;
  b.km_source = KM_NONE;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  create_run("2026-04");
  expect_blocker("km_pending", bid);
}

void test_finalize_blocked_no_tariff(void) {
  int64_t bid = add_done(ua, car2, APR10, 2, 0);
  create_run("2026-04");
  expect_blocker("no_tariff", bid);
}

void test_finalize_blocked_no_mandate(void) {
  int64_t ub = th_add_user_bare(a, "bare@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(ub > 0);
  add_done(ub, car1, APR10, 2, 10);
  create_run("2026-04");
  expect_blocker("no_mandate", ub);
}

void test_finalize_no_creditor_409(void) {
  add_april();
  int64_t id = create_run("2026-04");
  settings_t s = a->db.settings;
  s.creditor_iban[0] = 0;
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&a->db, &s));
  call_free("POST", rpath(id, "/finalize"), NULL, 409, "no_creditor");
  TEST_ASSERT_EQUAL_INT(RUN_DRAFT, a->db.runs[0].state);
}

void test_finalize_gapless_numbers(void) {
  add_april();
  add_done(uz, car1, MAY12, 1, 0);
  int64_t r1 = create_run("2026-04");
  cJSON *j = call("POST", rpath(r1, "/finalize"), NULL, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("final", obj(obj(j, "run"), "state")->valuestring);
  TEST_ASSERT_EQUAL_INT64(N, num(obj(j, "run"), "finalized_at"));
  cJSON *users = obj(j, "users");
  TEST_ASSERT_EQUAL_INT64(1, num(obj(cJSON_GetArrayItem(users, 0), "invoice"), "number"));
  TEST_ASSERT_EQUAL_INT64(ua, num(cJSON_GetArrayItem(users, 0), "user_id"));
  TEST_ASSERT_EQUAL_INT64(2, num(obj(cJSON_GetArrayItem(users, 1), "invoice"), "number"));
  TEST_ASSERT_EQUAL_INT64(0, num(obj(cJSON_GetArrayItem(users, 1), "invoice"), "mailed_at"));
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(obj(obj(j, "blockers"), "km_pending")));
  cJSON_Delete(j);
  const invoice_t *i1 = db_invoice(&a->db, 1);
  TEST_ASSERT_NOT_NULL(i1);
  TEST_ASSERT_EQUAL_STRING("Anna A", i1->name);
  TEST_ASSERT_EQUAL_INT64(1200, i1->total_cents);
  TEST_ASSERT_EQUAL_INT64(N, i1->created_at);
  TEST_ASSERT_EQUAL_STRING("2026-04", i1->period);
  int64_t r2 = create_run("2026-05");
  j = call("POST", rpath(r2, "/finalize"), NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(3, num(obj(cJSON_GetArrayItem(obj(j, "users"), 0), "invoice"), "number"));
  cJSON_Delete(j);
  j = call("GET", "/api/admin/billing", NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(r2, num(cJSON_GetArrayItem(j, 0), "id"));
  TEST_ASSERT_EQUAL_INT64(1, num(cJSON_GetArrayItem(j, 0), "n_invoices"));
  TEST_ASSERT_EQUAL_INT64(2, num(cJSON_GetArrayItem(j, 1), "n_invoices"));
  TEST_ASSERT_EQUAL_INT64(1800, num(cJSON_GetArrayItem(j, 1), "total_cents"));
  cJSON_Delete(j);
}

void test_finalize_locks_bookings(void) {
  int64_t bid = add_done(ua, car1, APR10, 2, 10);
  call_free("POST", rpath(create_run("2026-04"), "/finalize"), NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(1, db_booking(&a->db, bid)->invoice_id);
  char p[64], body[160];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)bid);
  snprintf(body, sizeof body, "{\"car_id\":%lld,\"user_id\":%lld,\"start\":%lld,\"end\":%lld}",
           (long long)car1, (long long)ua, (long long)APR10, (long long)(APR10 + 2 * H));
  call_free("PUT", p, body, 409, "invoiced");
  call_free("DELETE", p, NULL, 409, "invoiced");
  call_free("PUT", "/api/admin/bookings/9999", body, 404, "not_found");
}

void test_finalize_uses_current_state(void) {
  int64_t bid = add_done(ua, car1, APR10, 2, 10);
  int64_t id = create_run("2026-04");
  cJSON *j = detail(id);
  TEST_ASSERT_EQUAL_INT64(10, num(cJSON_GetArrayItem(obj(cJSON_GetArrayItem(obj(j, "users"), 0), "lines"), 0), "km"));
  cJSON_Delete(j);
  booking_t b = *db_booking(&a->db, bid);
  b.km_end = b.km_start + 40;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  j = call("POST", rpath(id, "/finalize"), NULL, 200, NULL);
  cJSON *line = cJSON_GetArrayItem(obj(cJSON_GetArrayItem(obj(j, "users"), 0), "lines"), 0);
  TEST_ASSERT_EQUAL_INT64(40, num(line, "km"));
  TEST_ASSERT_EQUAL_INT64(600 + 40 * 30, num(line, "total_cents"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT64(1800, db_invoice(&a->db, 1)->total_cents);
  TEST_ASSERT_EQUAL_INT64(40, a->db.lines[0].km);
}

void test_finalize_twice_409(void) {
  add_april();
  int64_t id = create_run("2026-04");
  call_free("POST", rpath(id, "/finalize"), NULL, 200, NULL);
  call_free("POST", rpath(id, "/finalize"), NULL, 409, "not_draft");
  call_free("DELETE", rpath(id, ""), NULL, 409, "not_draft");
  TEST_ASSERT_EQUAL_UINT(2, a->db.n_invoices);
  call_free("POST", rpath(999, "/finalize"), NULL, 404, "not_found");
}

void test_delete_draft(void) {
  add_april();
  int64_t id = create_run("2026-04");
  call_free("DELETE", rpath(id, ""), NULL, 200, NULL);
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_runs);
  call_free("GET", rpath(id, ""), NULL, 404, "not_found");
  create_run("2026-04");
}

void test_settings_lead_days_range(void) {
#define SETTINGS(lead, footer)                                                                    \
  "{\"start_tolerance_min\":15,\"cancel_deadline_min\":60,\"cancel_fee_cents\":500,"            \
  "\"assoc_name\":\"\",\"assoc_street\":\"\",\"assoc_zip\":\"\",\"assoc_city\":\"\","         \
  "\"creditor_id\":\"\",\"creditor_iban\":\"\",\"creditor_bic\":\"\"" lead footer "}"
  call_free("PUT", "/api/admin/settings", SETTINGS(",\"sepa_lead_days\":4", ",\"invoice_footer\":\"\""), 400, "invalid");
  call_free("PUT", "/api/admin/settings", SETTINGS(",\"sepa_lead_days\":31", ",\"invoice_footer\":\"\""), 400, "invalid");
  call_free("PUT", "/api/admin/settings", SETTINGS("", ",\"invoice_footer\":\"\""), 400, "invalid");
  call_free("PUT", "/api/admin/settings", SETTINGS(",\"sepa_lead_days\":5", ""), 400, "invalid");
  char big[700], body[1400];
  memset(big, 'x', 512);
  big[512] = 0;
  snprintf(body, sizeof body, "{\"start_tolerance_min\":15,\"cancel_deadline_min\":60,\"cancel_fee_cents\":500,"
           "\"assoc_name\":\"\",\"assoc_street\":\"\",\"assoc_zip\":\"\",\"assoc_city\":\"\","
           "\"creditor_id\":\"\",\"creditor_iban\":\"\",\"creditor_bic\":\"\",\"sepa_lead_days\":5,"
           "\"invoice_footer\":\"%s\"}", big);
  call_free("PUT", "/api/admin/settings", body, 400, "invalid");
  TEST_ASSERT_EQUAL_INT(5, a->db.settings.sepa_lead_days);
  cJSON *j = call("PUT", "/api/admin/settings",
                  SETTINGS(",\"sepa_lead_days\":30", ",\"invoice_footer\":\"  Danke\\nan alle \\t\""), 200, NULL);
  TEST_ASSERT_EQUAL_INT64(30, num(j, "sepa_lead_days"));
  TEST_ASSERT_EQUAL_STRING("Danke\nan alle", obj(j, "invoice_footer")->valuestring);
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_STRING("Danke\nan alle", a->db.settings.invoice_footer);
  j = call("GET", "/api/admin/settings", NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(30, num(j, "sepa_lead_days"));
  cJSON_Delete(j);
  call_free("PUT", "/api/admin/settings", SETTINGS(",\"sepa_lead_days\":5", ",\"invoice_footer\":\"\""), 200, NULL);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_create_requires_closed_month);
  RUN_TEST(test_create_draft_exists_409);
  RUN_TEST(test_create_not_ascending_409);
  RUN_TEST(test_draft_lists_users_lines_totals);
  RUN_TEST(test_draft_zero_invoice_club_only);
  RUN_TEST(test_draft_no_line_no_user);
  RUN_TEST(test_draft_straggler_included);
  RUN_TEST(test_finalize_blocked_km_pending);
  RUN_TEST(test_finalize_blocked_no_tariff);
  RUN_TEST(test_finalize_blocked_no_mandate);
  RUN_TEST(test_finalize_no_creditor_409);
  RUN_TEST(test_finalize_gapless_numbers);
  RUN_TEST(test_finalize_locks_bookings);
  RUN_TEST(test_finalize_uses_current_state);
  RUN_TEST(test_finalize_twice_409);
  RUN_TEST(test_delete_draft);
  RUN_TEST(test_settings_lead_days_range);
  return UNITY_END();
}
