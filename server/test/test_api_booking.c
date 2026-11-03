#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/booking_rules.h"
#include "auth/session.h"
#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400
#define H 3600

static app_t *a;
static int64_t u1, u2, car1, car2, car_off;
static char tok1[65], tok2[65];

static int64_t add_car(const char *name, bool active) {
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "%s", name);
  snprintf(c.plate, sizeof c.plate, "X-%s", name);
  snprintf(c.location, sizeof c.location, "Hof");
  c.active = active;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  return c.id;
}

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  /* No password: sessions are created directly, avoiding Argon2id cost. */
  u1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  u2 = th_add_user(a, "bob@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(u1 > 0 && u2 > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, T, tok1));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u2, T, tok2));
  car1 = add_car("Golf", true);
  car2 = add_car("Polo", true);
  car_off = add_car("Alt", false);
  settings_t s = a->db.settings; /* keeps codec defaults */
  s.start_tolerance_min = 15;
  s.cancel_deadline_min = 60;
  s.cancel_fee_cents = 500;
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&a->db, &s));
}

void tearDown(void) { th_free(a); }

static cJSON *call_at(const char *m, const char *p, const char *body, const char *tok, int status,
                      const char *code, int64_t now) {
  api_res_t r = th_req(a, m, p, body, tok, now);
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

static cJSON *call(const char *m, const char *p, const char *body, const char *tok, int status,
                   const char *code) {
  return call_at(m, p, body, tok, status, code, T);
}

static void call_free(const char *m, const char *p, const char *body, const char *tok, int status,
                      const char *code) {
  cJSON_Delete(call(m, p, body, tok, status, code));
}

static int64_t num(const cJSON *o, const char *k) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsNumber(v));
  return (int64_t)v->valuedouble;
}

static char *body3(int64_t car, int64_t s, int64_t e) {
  static char b[160];
  snprintf(b, sizeof b, "{\"car_id\":%lld,\"start\":%lld,\"end\":%lld}", (long long)car,
           (long long)s, (long long)e);
  return b;
}

static char *body2(int64_t s, int64_t e) {
  static char b[96];
  snprintf(b, sizeof b, "{\"start\":%lld,\"end\":%lld}", (long long)s, (long long)e);
  return b;
}

/* Creates a booking via the API; returns its id. */
static int64_t book(const char *tok, int64_t car, int64_t s, int64_t e) {
  cJSON *j = call("POST", "/api/bookings", body3(car, s, e), tok, 201, NULL);
  int64_t id = num(j, "id");
  cJSON_Delete(j);
  return id;
}

static char cal_path[200];
static const char *cal(int64_t car, int64_t from, int64_t to) {
  snprintf(cal_path, sizeof cal_path, "/api/calendar?car=%lld&from=%lld&to=%lld", (long long)car,
           (long long)from, (long long)to);
  return cal_path;
}

void test_auth_required(void) {
  call_free("GET", "/api/cars", NULL, NULL, 401, NULL);
  call_free("GET", "/api/bookings", NULL, NULL, 401, NULL);
  call_free("GET", "/api/settings/public", NULL, NULL, 401, NULL);
}

void test_settings_public(void) {
  cJSON *j = call("GET", "/api/settings/public", NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT(15, num(j, "start_tolerance_min"));
  TEST_ASSERT_EQUAL_INT(60, num(j, "cancel_deadline_min"));
  TEST_ASSERT_EQUAL_INT(500, num(j, "cancel_fee_cents"));
  cJSON_Delete(j);
}

void test_cars_lists_only_active(void) {
  cJSON *j = call("GET", "/api/cars", NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  cJSON *c = cJSON_GetArrayItem(j, 0);
  TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(c));
  TEST_ASSERT_EQUAL_INT(car1, num(c, "id"));
  TEST_ASSERT_EQUAL_STRING("Golf", cJSON_GetObjectItemCaseSensitive(c, "name")->valuestring);
  TEST_ASSERT_EQUAL_STRING("X-Golf", cJSON_GetObjectItemCaseSensitive(c, "plate")->valuestring);
  TEST_ASSERT_EQUAL_STRING("Hof", cJSON_GetObjectItemCaseSensitive(c, "location")->valuestring);
  TEST_ASSERT_EQUAL_INT(car2, num(cJSON_GetArrayItem(j, 1), "id"));
  cJSON_Delete(j);
}

void test_calendar_hides_other_users_details(void) {
  int64_t id1 = book(tok1, car1, T + 2 * H, T + 3 * H);
  book(tok2, car1, T + 5 * H, T + 6 * H);
  cJSON *j = call("GET", cal(car1, T, T + 86400), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  cJSON *own = cJSON_GetArrayItem(j, 0), *oth = cJSON_GetArrayItem(j, 1);
  TEST_ASSERT_EQUAL_INT(6, cJSON_GetArraySize(own));
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(own, "club_trip")));
  TEST_ASSERT_EQUAL_INT(id1, num(own, "id"));
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(own, "own")));
  TEST_ASSERT_EQUAL_STRING("reserved", cJSON_GetObjectItemCaseSensitive(own, "state")->valuestring);
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(oth));
  TEST_ASSERT_EQUAL_INT(T + 5 * H, num(oth, "start"));
  TEST_ASSERT_EQUAL_INT(T + 6 * H, num(oth, "end"));
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(oth, "own")));
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(oth, "id"));
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(oth, "state"));
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(oth, "club_trip"));
  cJSON_Delete(j);
  /* other car is empty */
  j = call("GET", cal(car2, T, T + 86400), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(j));
  cJSON_Delete(j);
}

/* Overdue ACTIVE stays occupied through now + grace, inside the [max(from,now), to) window. */
void test_calendar_active_and_overdue_listed(void) {
  int64_t id = book(tok2, car1, T, T + H);
  booking_t b = *db_booking(&a->db, id);
  b.state = BK_ACTIVE;
  b.booked_start = T - 2 * H;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  cJSON *j = call("GET", cal(car1, T - 5 * H, T + 86400), NULL, tok2, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  cJSON *e = cJSON_GetArrayItem(j, 0);
  TEST_ASSERT_EQUAL_INT(T + H, num(e, "end"));
  TEST_ASSERT_EQUAL_STRING("active", cJSON_GetObjectItemCaseSensitive(e, "state")->valuestring);
  cJSON_Delete(j);
  b.booked_end = T - H / 2;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  /* overdue active stays occupied through now + grace */
  j = call("GET", cal(car1, T - 5 * H, T + 86400), NULL, tok2, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT(T + BR_OVERDUE_GRACE, num(cJSON_GetArrayItem(j, 0), "end"));
  cJSON_Delete(j);
}

void test_calendar_hides_past_bookings(void) {
  int64_t id = book(tok1, car1, T + 2 * H, T + 3 * H);
  /* ended before now: from the db view it is in the past */
  booking_t b = *db_booking(&a->db, id);
  b.booked_start = T - 3 * H;
  b.booked_end = T - 2 * H;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  cJSON *j = call("GET", cal(car1, T - 86400, T + 86400), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(j));
  cJSON_Delete(j);
}

void test_calendar_cancelled_not_listed(void) {
  int64_t id = book(tok1, car1, T + 5 * H, T + 6 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  call_free("DELETE", p, NULL, tok1, 200, NULL);
  cJSON *j = call("GET", cal(car1, T, T + 86400), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(j));
  cJSON_Delete(j);
}

void test_calendar_range_limit_400(void) {
  call_free("GET", cal(car1, T, T + 62 * 86400), NULL, tok1, 200, NULL);
  call_free("GET", cal(car1, T, T + 62 * 86400 + 1), NULL, tok1, 400, "range_too_large");
  call_free("GET", cal(car1, T, T), NULL, tok1, 400, "invalid");
  call_free("GET", cal(car1, T + 1, T), NULL, tok1, 400, "invalid");
  call_free("GET", "/api/calendar?car=1&from=5", NULL, tok1, 400, "invalid");
  call_free("GET", "/api/calendar?car=x&from=5&to=9", NULL, tok1, 400, "invalid");
}

void test_create_booking_201_and_persisted(void) {
  cJSON *j = call("POST", "/api/bookings", body3(car1, T + H, T + 2 * H), tok1, 201, NULL);
  int64_t id = num(j, "id");
  TEST_ASSERT_TRUE(id > 0);
  TEST_ASSERT_EQUAL_INT(u1, num(j, "user_id"));
  TEST_ASSERT_EQUAL_INT(u1, num(j, "modified_by"));
  TEST_ASSERT_EQUAL_INT(T, num(j, "created_at"));
  TEST_ASSERT_EQUAL_INT(T, num(j, "modified_at"));
  TEST_ASSERT_EQUAL_STRING("reserved", cJSON_GetObjectItemCaseSensitive(j, "state")->valuestring);
  cJSON_Delete(j);
  db_t d;
  TEST_ASSERT_EQUAL_INT(0, db_load(&d, a->cfg.data_dir));
  const booking_t *b = db_booking(&d, id);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_EQUAL_INT64(T + H, b->booked_start);
  TEST_ASSERT_EQUAL_INT64(u1, b->user_id);
  db_free(&d);
  /* listed under /api/bookings */
  j = call("GET", "/api/bookings", NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  cJSON_Delete(j);
  j = call("GET", "/api/bookings", NULL, tok2, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(j));
  cJSON_Delete(j);
}

void test_bookings_list_sorted_and_filtered(void) {
  int64_t late = book(tok1, car1, T + 10 * H, T + 11 * H);
  int64_t early = book(tok1, car2, T + 2 * H, T + 3 * H);
  int64_t gone = book(tok1, car1, T + 4 * H, T + 5 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)gone);
  call_free("DELETE", p, NULL, tok1, 200, NULL);
  cJSON *j = call("GET", "/api/bookings", NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT(early, num(cJSON_GetArrayItem(j, 0), "id"));
  TEST_ASSERT_EQUAL_INT(late, num(cJSON_GetArrayItem(j, 1), "id"));
  cJSON_Delete(j);
}

void test_create_invalid_inputs(void) {
  call_free("POST", "/api/bookings", NULL, tok1, 400, "invalid");
  call_free("POST", "/api/bookings", "{\"car_id\":1}", tok1, 400, "invalid");
  call_free("POST", "/api/bookings", body3(car1, T + 2 * H, T + H), tok1, 400, "invalid");
  call_free("POST", "/api/bookings", body3(999, T + H, T + 2 * H), tok1, 400, "invalid");
  call_free("POST", "/api/bookings", body3(car1, T + H + 1, T + 2 * H), tok1, 400, "invalid");
}

void test_create_car_inactive_and_in_past(void) {
  call_free("POST", "/api/bookings", body3(car_off, T + H, T + 2 * H), tok1, 400, "car_inactive");
  cJSON *j = call("POST", "/api/bookings", body3(car1, T - H, T + H), tok1, 400, "in_past");
  TEST_ASSERT_EQUAL_STRING("Der Beginn liegt in der Vergangenheit.",
                           cJSON_GetObjectItemCaseSensitive(j, "message")->valuestring);
  cJSON_Delete(j);
  /* within start tolerance is allowed */
  call_free("POST", "/api/bookings", body3(car1, T - 600, T + H), tok1, 201, NULL);
}

void test_create_overlap_409(void) {
  book(tok1, car1, T + H, T + 3 * H);
  cJSON *j = call("POST", "/api/bookings", body3(car1, T + 2 * H, T + 4 * H), tok2, 409, "overlap");
  TEST_ASSERT_EQUAL_STRING("Das Auto ist in diesem Zeitraum bereits gebucht.",
                           cJSON_GetObjectItemCaseSensitive(j, "message")->valuestring);
  cJSON_Delete(j);
  /* adjacent is fine */
  call_free("POST", "/api/bookings", body3(car1, T + 3 * H, T + 4 * H), tok2, 201, NULL);
}

void test_extend_ok_shorten_400(void) {
  int64_t id = book(tok1, car1, T + 2 * H, T + 3 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  cJSON *j = call("PUT", p, body2(T + 2 * H, T + 4 * H), tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(T + 4 * H, num(j, "booked_end"));
  TEST_ASSERT_EQUAL_INT(u1, num(j, "modified_by"));
  cJSON_Delete(j);
  j = call("PUT", p, body2(T + 2 * H, T + 3 * H), tok1, 400, "only_extend");
  TEST_ASSERT_EQUAL_STRING(
      "Buchungen können nur verlängert werden. Zum Verkürzen bitte stornieren und neu buchen.",
      cJSON_GetObjectItemCaseSensitive(j, "message")->valuestring);
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT64(T + 4 * H, db_booking(&a->db, id)->booked_end);
  call_free("PUT", p, "{\"start\":1}", tok1, 400, "invalid");
  call_free("PUT", p, NULL, tok1, 400, "invalid");
}

void test_extend_overlap_409(void) {
  int64_t id = book(tok1, car1, T + 2 * H, T + 3 * H);
  book(tok2, car1, T + 4 * H, T + 5 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  call_free("PUT", p, body2(T + 2 * H, T + 4 * H + 60), tok1, 409, "overlap");
}

void test_cancel_sets_state_and_fee(void) {
  int64_t far = book(tok1, car1, T + 5 * H, T + 6 * H);
  int64_t near = book(tok1, car1, T + 30 * 60, T + H);
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)far);
  cJSON *j = call("DELETE", p, NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("cancelled", cJSON_GetObjectItemCaseSensitive(j, "state")->valuestring);
  TEST_ASSERT_EQUAL_INT(0, num(j, "cancel_fee_cents"));
  TEST_ASSERT_EQUAL_INT(T, num(j, "cancelled_at"));
  cJSON_Delete(j);
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)near);
  j = call("DELETE", p, NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(500, num(j, "cancel_fee_cents"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(BK_CANCELLED, db_booking(&a->db, near)->state);
  /* cancelled: not modifiable, not cancellable again */
  call_free("DELETE", p, NULL, tok1, 400, "not_modifiable");
  call_free("PUT", p, body2(T + 30 * 60, T + 2 * H), tok1, 400, "not_modifiable");
}

void test_cancel_after_start_in_past(void) {
  int64_t id = book(tok1, car1, T + H, T + 2 * H);
  booking_t b = *db_booking(&a->db, id);
  b.booked_start = T - H;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  call_free("DELETE", p, NULL, tok1, 400, "in_past");
}

void test_other_users_booking_404(void) {
  int64_t id = book(tok1, car1, T + 2 * H, T + 3 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  call_free("PUT", p, body2(T + 2 * H, T + 4 * H), tok2, 404, "not_found");
  call_free("DELETE", p, NULL, tok2, 404, "not_found");
  call_free("DELETE", "/api/bookings/9999", NULL, tok2, 404, "not_found");
  call_free("PUT", "/api/bookings/9999", body2(T, T + H), tok2, 404, "not_found");
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, db_booking(&a->db, id)->state);
}

void test_path_id_not_numeric_400(void) {
  call_free("PUT", "/api/bookings/abc", body2(T, T + H), tok1, 400, "invalid");
  call_free("DELETE", "/api/bookings/abc", NULL, tok1, 400, "invalid");
}

void test_storage_failure_500_no_phantom(void) {
  if (geteuid() == 0) TEST_IGNORE_MESSAGE("root ignores directory permissions");
  TEST_ASSERT_EQUAL_INT(0, chmod(a->cfg.data_dir, 0500));
  api_res_t r = th_req(a, "POST", "/api/bookings", body3(car1, T + H, T + 2 * H), tok1, T);
  int st = r.status;
  char *code = r.body ? strstr(r.body, "\"storage\"") : NULL;
  int has_code = code != NULL;
  api_res_free(&r);
  chmod(a->cfg.data_dir, 0700);
  TEST_ASSERT_EQUAL_INT(500, st);
  TEST_ASSERT_TRUE(has_code);
  cJSON *j = call("GET", cal(car1, T, T + 86400), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(j));
  cJSON_Delete(j);
}

/* C1: an over-range time must be rejected and must never reach the data dir. */
static void assert_dir_loads(void) {
  db_t d;
  TEST_ASSERT_EQUAL_INT(0, db_load(&d, a->cfg.data_dir));
  db_free(&d);
}

#define YEAR2 (2 * 365 * 86400)

void test_create_time_out_of_range_400(void) {
  char b[200];
  snprintf(b, sizeof b, "{\"car_id\":%lld,\"start\":%lld,\"end\":4323455642275676160}",
           (long long)car1, (long long)(T + H));
  call_free("POST", "/api/bookings", b, tok1, 400, "invalid");
  call_free("POST", "/api/bookings", body3(car1, T + H, T + YEAR2 + 60), tok1, 400, "invalid");
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_bookings);
  assert_dir_loads();
  cJSON *j = call("GET", cal(car1, T, T + 86400), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(j));
  cJSON_Delete(j);
  /* the upper bound itself is allowed */
  book(tok1, car1, T + YEAR2 - H, T + YEAR2);
  assert_dir_loads();
}

void test_extend_time_out_of_range_400(void) {
  int64_t id = book(tok1, car1, T + H, T + 2 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  call_free("PUT", p, "{\"start\":1774746000,\"end\":4323455642275676160}", tok1, 400, "invalid");
  call_free("PUT", p, body2(T + H, T + YEAR2 + 60), tok1, 400, "invalid");
  TEST_ASSERT_EQUAL_INT64(T + 2 * H, db_booking(&a->db, id)->booked_end);
  assert_dir_loads();
}

static char *club_body(int64_t car, int64_t s, int64_t e, const char *flag) {
  static char b[200];
  snprintf(b, sizeof b, "{\"car_id\":%lld,\"start\":%lld,\"end\":%lld,\"club_trip\":%s}",
           (long long)car, (long long)s, (long long)e, flag);
  return b;
}
static char *club_put(int64_t s, int64_t e, const char *flag) {
  static char b[160];
  snprintf(b, sizeof b, "{\"start\":%lld,\"end\":%lld,\"club_trip\":%s}", (long long)s,
           (long long)e, flag);
  return b;
}
static void allow_club(int64_t uid) {
  user_t u = *db_user(&a->db, uid);
  u.may_club_trip = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
}
static const char *path_of(int64_t id) {
  static char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  return p;
}

void test_club_create_forbidden_without_permission(void) {
  cJSON *j = call("POST", "/api/bookings", club_body(car1, T + 2 * H, T + 3 * H, "true"), tok1, 403,
                  "forbidden");
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_bookings);
  j = call("POST", "/api/bookings", club_body(car1, T + 2 * H, T + 3 * H, "false"), tok1, 201, NULL);
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "club_trip")));
  cJSON_Delete(j);
  call_free("POST", "/api/bookings", club_body(car1, T + 5 * H, T + 6 * H, "1"), tok1, 400, "invalid");
}

void test_club_create_ok_with_permission(void) {
  allow_club(u1);
  cJSON *j = call("POST", "/api/bookings", club_body(car1, T + 2 * H, T + 3 * H, "true"), tok1, 201, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "club_trip")));
  int64_t id = num(j, "id");
  cJSON_Delete(j);
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->club_trip);
  /* other rules still apply */
  call_free("POST", "/api/bookings", club_body(car1, T + 2 * H, T + 3 * H, "true"), tok1, 409, "overlap");
  /* own list and calendar show the flag, others do not */
  j = call("GET", "/api/bookings", NULL, tok1, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(j, 0), "club_trip")));
  cJSON_Delete(j);
  j = call("GET", cal(car1, T, T + 86400), NULL, tok1, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(j, 0), "club_trip")));
  cJSON_Delete(j);
  j = call("GET", cal(car1, T, T + 86400), NULL, tok2, 200, NULL);
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(cJSON_GetArrayItem(j, 0)));
  cJSON_Delete(j);
}

void test_club_update_flag_before_start(void) {
  int64_t id = book(tok1, car1, T + 2 * H, T + 3 * H);
  call_free("PUT", path_of(id), club_put(T + 2 * H, T + 3 * H, "true"), tok1, 403, "forbidden");
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->club_trip);
  allow_club(u1);
  /* flag only, times unchanged */
  cJSON *j = call("PUT", path_of(id), club_put(T + 2 * H, T + 3 * H, "true"), tok1, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "club_trip")));
  cJSON_Delete(j);
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->club_trip);
  /* omitted flag = unchanged, also on extension */
  call_free("PUT", path_of(id), body2(T + 2 * H, T + 4 * H), tok1, 200, NULL);
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->club_trip);
  /* unchanged times and flag: still only_extend */
  call_free("PUT", path_of(id), club_put(T + 2 * H, T + 4 * H, "true"), tok1, 400, "only_extend");
  /* clear, also after revoking permission */
  user_t u = *db_user(&a->db, u1);
  u.may_club_trip = false;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
  call_free("PUT", path_of(id), club_put(T + 2 * H, T + 4 * H, "false"), tok1, 200, NULL);
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->club_trip);
}

void test_club_update_flag_after_start_400(void) {
  allow_club(u1);
  int64_t id = book(tok1, car1, T + 2 * H, T + 3 * H);
  call_free("PUT", path_of(id), club_put(T + 2 * H, T + 3 * H, "true"), tok1, 200, NULL);
  booking_t b = *db_booking(&a->db, id);
  b.state = BK_ACTIVE;
  b.booked_start = T - H;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  call_free("PUT", path_of(id), club_put(T - H, T + 3 * H, "false"), tok1, 400, "not_modifiable");
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->club_trip);
  call_free("PUT", path_of(id), body2(T - H, T + 4 * H), tok1, 200, NULL); /* extension ok */
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->club_trip);
}

void test_club_cancel_fee_zero_inside_deadline(void) {
  allow_club(u1);
  int64_t id = 0;
  cJSON *j = call("POST", "/api/bookings", club_body(car1, T + 30 * 60, T + H, "true"), tok1, 201, NULL);
  id = num(j, "id");
  cJSON_Delete(j);
  j = call("DELETE", path_of(id), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, num(j, "cancel_fee_cents"));
  cJSON_Delete(j);
}

void test_club_admin_on_user_endpoint_needs_permission(void) {
  char tadm[65];
  int64_t adm = th_add_user(a, "adm@x.de", NULL, ROLE_ADMIN);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tadm));
  call_free("POST", "/api/bookings", club_body(car1, T + 2 * H, T + 3 * H, "true"), tadm, 403, "forbidden");
  allow_club(adm);
  call_free("POST", "/api/bookings", club_body(car1, T + 2 * H, T + 3 * H, "true"), tadm, 201, NULL);
}

static const char *start_path(int64_t id) {
  static char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld/start", (long long)id);
  return p;
}

static const char *return_path(int64_t id) {
  static char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld/return", (long long)id);
  return p;
}

/* Puts a booking straight into the db in the given state. */
static int64_t put_booking(int64_t user, int64_t car, int64_t s, int64_t e, booking_state_t st) {
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = car;
  b.user_id = user;
  b.booked_start = s;
  b.booked_end = e;
  b.state = st;
  b.created_at = b.modified_at = T;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  return b.id;
}

void test_start_and_return_flow(void) {
  int64_t id = book(tok1, car1, T + 600, T + 2 * H);
  cJSON *j = call("POST", start_path(id), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("active", cJSON_GetObjectItemCaseSensitive(j, "state")->valuestring);
  TEST_ASSERT_EQUAL_INT(T, num(j, "actual_start"));
  cJSON_Delete(j);
  j = call_at("POST", return_path(id), NULL, tok1, 200, NULL, T + H);
  TEST_ASSERT_EQUAL_STRING("done", cJSON_GetObjectItemCaseSensitive(j, "state")->valuestring);
  TEST_ASSERT_EQUAL_INT(T + H, num(j, "actual_end"));
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "km_pending")));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(BK_DONE, db_booking(&a->db, id)->state);
}

void test_return_keeps_manual_km(void) {
  int64_t id = put_booking(u1, car1, T - H, T + H, BK_ACTIVE);
  booking_t b = *db_booking(&a->db, id);
  b.actual_start = T - H;
  b.km_source = KM_MANUAL;
  b.km_start = 100;
  b.km_end = 150;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  cJSON *j = call_at("POST", return_path(id), NULL, tok1, 200, NULL, T);
  TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "km_pending")));
  TEST_ASSERT_EQUAL_STRING("manual", cJSON_GetObjectItemCaseSensitive(j, "km_source")->valuestring);
  TEST_ASSERT_EQUAL_INT(100, num(j, "km_start"));
  TEST_ASSERT_EQUAL_INT(150, num(j, "km_end"));
  cJSON_Delete(j);
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->km_pending);
}

void test_start_twice_400(void) {
  int64_t id = book(tok1, car1, T + 600, T + 2 * H);
  call_free("POST", start_path(id), NULL, tok1, 200, NULL);
  call_free("POST", start_path(id), NULL, tok1, 400, "not_modifiable");
}

void test_return_reserved_400(void) {
  int64_t id = book(tok1, car1, T + 600, T + 2 * H);
  call_free("POST", return_path(id), NULL, tok1, 400, "not_modifiable");
}

void test_start_other_users_booking_404(void) {
  int64_t id = book(tok1, car1, T + 600, T + 2 * H);
  call_free("POST", start_path(id), NULL, tok2, 404, "not_found");
  call_free("POST", return_path(id), NULL, tok2, 404, "not_found");
}

void test_start_blocked_409_car_busy(void) {
  put_booking(u2, car1, T - 2 * H, T - H, BK_ACTIVE);
  int64_t id = book(tok1, car1, T + H, T + 2 * H);
  call_free("POST", start_path(id), NULL, tok1, 409, "car_busy");
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, db_booking(&a->db, id)->state);
}

void test_start_expired_400(void) {
  int64_t id = book(tok1, car1, T + 600, T + 2 * H);
  cJSON_Delete(call_at("POST", start_path(id), NULL, tok1, 400, "expired", T + 2 * H));
}

void test_cancel_after_start_rejected(void) {
  int64_t id = book(tok1, car1, T + 600, T + 2 * H);
  call_free("POST", start_path(id), NULL, tok1, 200, NULL);
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  call_free("DELETE", p, NULL, tok1, 400, "not_modifiable");
}

void test_extend_active_ok(void) {
  int64_t id = book(tok1, car1, T + 600, T + 2 * H);
  call_free("POST", start_path(id), NULL, tok1, 200, NULL);
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  cJSON *j = call("PUT", p, body2(T + 600, T + 3 * H), tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(T + 3 * H, num(j, "booked_end"));
  cJSON_Delete(j);
}

void test_calendar_shows_overdue_active(void) {
  put_booking(u2, car1, T - 2 * H, T - H, BK_ACTIVE);
  cJSON *j = call("GET", cal(car1, T, T + 86400), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  cJSON *e = cJSON_GetArrayItem(j, 0);
  TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(e, "own")));
  TEST_ASSERT_EQUAL_INT(T + BR_OVERDUE_GRACE, num(e, "end"));
  cJSON_Delete(j);
}

void test_calendar_shows_early_start(void) {
  int64_t id = put_booking(u2, car1, T + 2 * H, T + 3 * H, BK_ACTIVE);
  booking_t b = *db_booking(&a->db, id);
  b.actual_start = T + 600;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  /* window ends before booked_start: only the early start makes it overlap */
  cJSON *j = call("GET", cal(car1, T, T + H), NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT(T + 600, num(cJSON_GetArrayItem(j, 0), "start"));
  cJSON_Delete(j);
}

void test_history_scope(void) {
  int64_t d1 = put_booking(u1, car1, T - 5 * H, T - 4 * H, BK_DONE);
  int64_t c1 = put_booking(u1, car1, T - 3 * H, T - 2 * H, BK_CANCELLED);
  int64_t d2 = put_booking(u1, car2, T - 9 * H, T - 8 * H, BK_DONE);
  put_booking(u1, car1, T + 2 * H, T + 3 * H, BK_RESERVED);
  put_booking(u2, car1, T - 7 * H, T - 6 * H, BK_DONE);
  cJSON *j = call("GET", "/api/bookings?scope=history", NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT(c1, num(cJSON_GetArrayItem(j, 0), "id"));
  TEST_ASSERT_EQUAL_INT(d1, num(cJSON_GetArrayItem(j, 1), "id"));
  TEST_ASSERT_EQUAL_INT(d2, num(cJSON_GetArrayItem(j, 2), "id"));
  cJSON_Delete(j);
  j = call("GET", "/api/bookings?scope=upcoming", NULL, tok1, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  cJSON_Delete(j);
}

void test_scope_invalid_400(void) {
  call_free("GET", "/api/bookings?scope=all", NULL, tok1, 400, "invalid");
  call_free("GET", "/api/bookings?scope=", NULL, tok1, 400, "invalid");
}

void test_expire_sweep(void) {
  int64_t r = put_booking(u1, car1, T - 2 * H, T - 60, BK_RESERVED);
  int64_t fut = put_booking(u1, car1, T + H, T + 2 * H, BK_RESERVED);
  int64_t act = put_booking(u2, car2, T - 2 * H, T - 60, BK_ACTIVE);
  TEST_ASSERT_EQUAL_INT(1, api_expire_bookings(a, T));
  const booking_t *b = db_booking(&a->db, r);
  TEST_ASSERT_EQUAL_INT(BK_DONE, b->state);
  TEST_ASSERT_EQUAL_INT(0, b->actual_start);
  TEST_ASSERT_FALSE(b->km_pending);
  TEST_ASSERT_EQUAL_INT(T, b->modified_at);
  TEST_ASSERT_EQUAL_INT(0, b->modified_by);
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, db_booking(&a->db, fut)->state);
  TEST_ASSERT_EQUAL_INT(BK_ACTIVE, db_booking(&a->db, act)->state);
  TEST_ASSERT_EQUAL_INT(0, api_expire_bookings(a, T));
}

/* Phase 5: bookings need a current SEPA mandate. */
static int64_t bare_user(const char *email, char *tok) {
  int64_t id = th_add_user_bare(a, email, NULL, ROLE_USER);
  TEST_ASSERT_TRUE(id > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, id, T, tok));
  return id;
}

void test_create_without_mandate_403(void) {
  char tb[65];
  int64_t ub = bare_user("bare@x.de", tb);
  call_free("POST", "/api/bookings", body3(car1, T + 2 * H, T + 3 * H), tb, 403, "no_mandate");
  allow_club(ub);
  call_free("POST", "/api/bookings", club_body(car1, T + 2 * H, T + 3 * H, "true"), tb, 403,
            "no_mandate");
  /* gate runs before body parsing */
  call_free("POST", "/api/bookings", "{}", tb, 403, "no_mandate");
}

void test_put_and_start_without_mandate_403(void) {
  char tb[65];
  int64_t ub = bare_user("bare@x.de", tb);
  int64_t id = put_booking(ub, car1, T + 600, T + 2 * H, BK_RESERVED);
  call_free("PUT", path_of(id), body2(T + 600, T + 3 * H), tb, 403, "no_mandate");
  call_free("POST", start_path(id), NULL, tb, 403, "no_mandate");
  call_free("DELETE", path_of(id), NULL, tb, 200, NULL);
}

void test_admin_creates_booking_for_user_without_mandate(void) {
  char tadm[65], tb[65];
  int64_t adm = th_add_user(a, "adm@x.de", NULL, ROLE_ADMIN);
  int64_t ub = bare_user("bare@x.de", tb);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tadm));
  char body[200];
  snprintf(body, sizeof body, "{\"car_id\":%lld,\"user_id\":%lld,\"start\":%lld,\"end\":%lld}",
           (long long)car1, (long long)ub, (long long)(T + 2 * H), (long long)(T + 3 * H));
  call_free("POST", "/api/admin/bookings", body, tadm, 201, NULL);
}

void test_foreign_booking_still_404(void) {
  char tb[65];
  bare_user("bare@x.de", tb);
  int64_t id = put_booking(u1, car1, T + 600, T + 2 * H, BK_RESERVED);
  call_free("PUT", path_of(id), body2(T + 600, T + 3 * H), tb, 404, NULL);
  call_free("POST", start_path(id), NULL, tb, 404, NULL);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_auth_required);
  RUN_TEST(test_settings_public);
  RUN_TEST(test_cars_lists_only_active);
  RUN_TEST(test_calendar_hides_other_users_details);
  RUN_TEST(test_calendar_active_and_overdue_listed);
  RUN_TEST(test_calendar_hides_past_bookings);
  RUN_TEST(test_calendar_cancelled_not_listed);
  RUN_TEST(test_calendar_range_limit_400);
  RUN_TEST(test_create_booking_201_and_persisted);
  RUN_TEST(test_bookings_list_sorted_and_filtered);
  RUN_TEST(test_create_invalid_inputs);
  RUN_TEST(test_create_car_inactive_and_in_past);
  RUN_TEST(test_create_overlap_409);
  RUN_TEST(test_extend_ok_shorten_400);
  RUN_TEST(test_extend_overlap_409);
  RUN_TEST(test_cancel_sets_state_and_fee);
  RUN_TEST(test_cancel_after_start_in_past);
  RUN_TEST(test_other_users_booking_404);
  RUN_TEST(test_path_id_not_numeric_400);
  RUN_TEST(test_storage_failure_500_no_phantom);
  RUN_TEST(test_create_time_out_of_range_400);
  RUN_TEST(test_extend_time_out_of_range_400);
  RUN_TEST(test_club_create_forbidden_without_permission);
  RUN_TEST(test_club_create_ok_with_permission);
  RUN_TEST(test_club_update_flag_before_start);
  RUN_TEST(test_club_update_flag_after_start_400);
  RUN_TEST(test_club_cancel_fee_zero_inside_deadline);
  RUN_TEST(test_club_admin_on_user_endpoint_needs_permission);
  RUN_TEST(test_start_and_return_flow);
  RUN_TEST(test_return_keeps_manual_km);
  RUN_TEST(test_start_twice_400);
  RUN_TEST(test_return_reserved_400);
  RUN_TEST(test_start_other_users_booking_404);
  RUN_TEST(test_start_blocked_409_car_busy);
  RUN_TEST(test_start_expired_400);
  RUN_TEST(test_cancel_after_start_rejected);
  RUN_TEST(test_extend_active_ok);
  RUN_TEST(test_calendar_shows_overdue_active);
  RUN_TEST(test_calendar_shows_early_start);
  RUN_TEST(test_history_scope);
  RUN_TEST(test_scope_invalid_400);
  RUN_TEST(test_expire_sweep);
  RUN_TEST(test_create_without_mandate_403);
  RUN_TEST(test_put_and_start_without_mandate_403);
  RUN_TEST(test_admin_creates_booking_for_user_without_mandate);
  RUN_TEST(test_foreign_booking_still_404);
  return UNITY_END();
}
