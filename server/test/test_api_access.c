#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/session.h"
#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "http/api_internal.h"
#include "unity.h"

#define T 1774742400
#define H 3600

#define PIN_MSG                                                                              \
  "PIN für die Schlüsselbox konnte nicht abgerufen werden. Bitte erneut versuchen. Klappt " \
  "es weiter nicht: Notfallkontakt auf der Hilfe-Seite."

static app_t *a;
static int64_t u1, u2, adm, car1, car2;
static char tok1[65], tok2[65], tokadm[65];

static int64_t add_car(const char *name, const char *lock) {
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "%s", name);
  snprintf(c.plate, sizeof c.plate, "X-%s", name);
  snprintf(c.location, sizeof c.location, "Hof");
  snprintf(c.lock_device_id, sizeof c.lock_device_id, "%s", lock);
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  return c.id;
}

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  u1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  u2 = th_add_user(a, "bob@x.de", NULL, ROLE_USER);
  adm = th_add_user(a, "admin@x.de", NULL, ROLE_ADMIN);
  TEST_ASSERT_TRUE(u1 > 0 && u2 > 0 && adm > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, T, tok1));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u2, T, tok2));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tokadm));
  car1 = add_car("Golf", "D1");
  car2 = add_car("Polo", "");
  settings_t s = a->db.settings;
  s.start_tolerance_min = 15;
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

static void call_free(const char *m, const char *p, const char *body, const char *tok, int status,
                      const char *code, int64_t now) {
  cJSON_Delete(call_at(m, p, body, tok, status, code, now));
}

static int64_t num(const cJSON *o, const char *k) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsNumber(v));
  return (int64_t)v->valuedouble;
}

static const char *str(const cJSON *o, const char *k) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsString(v));
  return v->valuestring;
}

static int64_t book(const char *tok, int64_t car, int64_t s, int64_t e) {
  char b[160];
  snprintf(b, sizeof b, "{\"car_id\":%lld,\"start\":%lld,\"end\":%lld}", (long long)car,
           (long long)s, (long long)e);
  cJSON *j = call_at("POST", "/api/bookings", b, tok, 201, NULL, T);
  int64_t id = num(j, "id");
  cJSON_Delete(j);
  return id;
}

static const char *path(int64_t id, const char *what) {
  static char p[96];
  snprintf(p, sizeof p, "/api/bookings/%lld/%s", (long long)id, what);
  return p;
}

static const char *admin_path(int64_t id) {
  static char p[96];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)id);
  return p;
}

static const char *admin_body(int64_t car, int64_t user, int64_t s, int64_t e) {
  static char b[160];
  snprintf(b, sizeof b, "{\"car_id\":%lld,\"user_id\":%lld,\"start\":%lld,\"end\":%lld}",
           (long long)car, (long long)user, (long long)s, (long long)e);
  return b;
}

static void start_ok(int64_t id, int64_t now) {
  call_free("POST", path(id, "start"), NULL, tok1, 200, NULL, now);
}

/* Raw body of a request (caller frees). */
static char *raw(const char *m, const char *p, const char *tok, int status, int64_t now) {
  api_res_t r = th_req(a, m, p, NULL, tok, now);
  TEST_ASSERT_NOT_NULL(r.body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(status, r.status, r.body);
  char *s = strdup(r.body);
  api_res_free(&r);
  TEST_ASSERT_NOT_NULL(s);
  return s;
}

static void assert_pins_blank(int64_t booking_id) {
  int n = 0;
  for (size_t i = 0; i < a->db.n_pins; i++)
    if (a->db.pins[i].booking_id == booking_id) {
      TEST_ASSERT_EQUAL_STRING("", a->db.pins[i].pin);
      n++;
    }
  TEST_ASSERT_TRUE(n > 0);
  /* And on disk. */
  db_t d;
  TEST_ASSERT_EQUAL_INT(0, db_load(&d, a->cfg.data_dir));
  for (size_t i = 0; i < d.n_pins; i++)
    if (d.pins[i].booking_id == booking_id) TEST_ASSERT_EQUAL_STRING("", d.pins[i].pin);
  db_free(&d);
}

void test_start_creates_pin(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  cJSON *j = call_at("POST", path(id, "start"), NULL, tok1, 200, NULL, T + H + 1200);
  TEST_ASSERT_EQUAL_STRING("active", str(j, "state"));
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(j, "pin"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, th_pin_count());
  TEST_ASSERT_EQUAL_STRING("D1", th_pin_last_device());
  TEST_ASSERT_EQUAL_INT64(T + H, th_pin_last_from());
  TEST_ASSERT_EQUAL_INT64(T + 5 * H, th_pin_last_until());
  TEST_ASSERT_EQUAL_INT(1, th_pin_last_variance());
  char name[48];
  snprintf(name, sizeof name, "Buchung %lld", (long long)id);
  TEST_ASSERT_EQUAL_STRING(name, th_pin_last_name());
  TEST_ASSERT_EQUAL_UINT(1, a->db.n_pins);
  const access_pin_t *p = &a->db.pins[0];
  TEST_ASSERT_EQUAL_INT64(id, p->booking_id);
  TEST_ASSERT_EQUAL_INT64(car1, p->car_id);
  TEST_ASSERT_EQUAL_STRING("P1", p->pin_id);
  TEST_ASSERT_EQUAL_STRING("100001", p->pin);
  TEST_ASSERT_EQUAL_INT(1, p->variance);
  TEST_ASSERT_EQUAL_INT64(T + H + 1200, p->created_at);
  j = call_at("GET", path(id, "pin"), NULL, tok1, 200, NULL, T + H + 1200);
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_STRING("100001", str(j, "pin"));
  TEST_ASSERT_EQUAL_INT64(T + H, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T + 5 * H, num(j, "valid_until"));
  cJSON_Delete(j);
}

void test_start_without_lock_unchanged(void) {
  int64_t id = book(tok1, car2, T + H, T + 5 * H);
  start_ok(id, T + H);
  TEST_ASSERT_EQUAL_INT(0, th_pin_count());
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_pins);
  call_free("GET", path(id, "pin"), NULL, tok1, 404, "not_found", T + H);
  call_free("POST", path(id, "pin"), "{}", tok1, 404, "not_found", T + H);
}

void test_start_pin_failure_keeps_reserved(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  th_pin_fail(PIN_ERR_NETWORK);
  cJSON *j = call_at("POST", path(id, "start"), NULL, tok1, 503, "pin_unavailable", T + H);
  TEST_ASSERT_EQUAL_STRING(PIN_MSG, str(j, "message"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, db_booking(&a->db, id)->state);
  TEST_ASSERT_EQUAL_INT64(0, db_booking(&a->db, id)->actual_start);
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_pins);
}

void test_start_malformed_pin_keeps_reserved(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  th_pin_malformed(1);
  cJSON *j = call_at("POST", path(id, "start"), NULL, tok1, 503, "pin_unavailable", T + H);
  TEST_ASSERT_EQUAL_STRING(PIN_MSG, str(j, "message"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, db_booking(&a->db, id)->state);
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_pins);
}

void test_failure_log_line(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  char file[300];
  snprintf(file, sizeof file, "%s/stderr.txt", a->cfg.data_dir);
  fflush(stderr);
  int saved = dup(2);
  int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  TEST_ASSERT_TRUE(saved >= 0 && fd >= 0);
  dup2(fd, 2);
  close(fd);
  th_pin_fail(PIN_ERR_NETWORK);
  api_res_t r = th_req(a, "POST", path(id, "start"), NULL, tok1, T + H);
  th_pin_fail(PIN_ERR_DEVICE);
  api_res_t r2 = th_req(a, "POST", path(id, "start"), NULL, tok1, T + H + 60);
  fflush(stderr);
  dup2(saved, 2);
  close(saved);
  TEST_ASSERT_EQUAL_INT(503, r.status);
  TEST_ASSERT_EQUAL_INT(503, r2.status);
  api_res_free(&r);
  api_res_free(&r2);
  char buf[512] = {0};
  FILE *f = fopen(file, "r");
  TEST_ASSERT_NOT_NULL(f);
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  char want[512];
  snprintf(want, sizeof want,
           "ocs-server: pin for booking %lld failed: igloo: unreachable (HTTP 0)\n"
           "ocs-server: pin for booking %lld failed: igloo: unknown device (HTTP 404)\n",
           (long long)id, (long long)id);
  TEST_ASSERT_EQUAL_STRING(want, buf);
}

void test_pin_get_foreign_404(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  /* Reserved: no PIN yet. */
  call_free("GET", path(id, "pin"), NULL, tok1, 404, "not_found", T + H);
  start_ok(id, T + H);
  char *s = raw("GET", path(id, "pin"), tok2, 404, T + H);
  TEST_ASSERT_NULL(strstr(s, "pin"));
  free(s);
  call_free("POST", path(id, "pin"), "{}", tok2, 404, "not_found", T + H);
  call_free("GET", path(id, "pin"), NULL, NULL, 401, NULL, T + H);
  call_free("GET", "/api/bookings/x/pin", NULL, tok1, 400, "invalid", T + H);
  call_free("GET", "/api/bookings/999/pin", NULL, tok1, 404, "not_found", T + H);
  TEST_ASSERT_EQUAL_INT(1, th_pin_count());
}

void test_pin_get_after_return_404(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H);
  call_free("POST", path(id, "return"), NULL, tok1, 200, NULL, T + 2 * H);
  call_free("GET", path(id, "pin"), NULL, tok1, 404, "not_found", T + 2 * H);
  call_free("POST", path(id, "pin"), "{}", tok1, 404, "not_found", T + 2 * H);
  assert_pins_blank(id);
  TEST_ASSERT_EQUAL_STRING("P1", a->db.pins[0].pin_id);
  TEST_ASSERT_EQUAL_INT64(T + 5 * H, a->db.pins[0].valid_until);
}

void test_pin_get_expired_null(void) {
  int64_t id = book(tok1, car1, T + H, T + 2 * H);
  start_ok(id, T + H);
  /* valid_until T+2H: usable until T+2H-900. */
  cJSON *j = call_at("GET", path(id, "pin"), NULL, tok1, 200, NULL, T + 2 * H - 901);
  TEST_ASSERT_EQUAL_STRING("100001", str(j, "pin"));
  cJSON_Delete(j);
  j = call_at("GET", path(id, "pin"), NULL, tok1, 200, NULL, T + 2 * H - 900);
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "pin")));
  TEST_ASSERT_EQUAL_INT64(T + H, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T + 2 * H, num(j, "valid_until"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, th_pin_count());
}

void test_pin_get_no_rows_zeros(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H);
  /* Rows pruned (or lost) while the trip runs. */
  TEST_ASSERT_EQUAL_INT(0, db_prune_pins(&a->db, T + 3 * H));
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_pins);
  cJSON *j = call_at("GET", path(id, "pin"), NULL, tok1, 200, NULL, T + 2 * H);
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "pin")));
  TEST_ASSERT_EQUAL_INT64(0, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(0, num(j, "valid_until"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, th_pin_count());
}

void test_pin_post_usable_returns_same(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H + 1200);
  cJSON *j = call_at("POST", path(id, "pin"), "{}", tok1, 200, NULL, T + 5 * H - 901);
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_STRING("100001", str(j, "pin"));
  TEST_ASSERT_EQUAL_INT64(T + H, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T + 5 * H, num(j, "valid_until"));
  cJSON_Delete(j);
  j = call_at("POST", path(id, "pin"), NULL, tok1, 200, NULL, T + 2 * H);
  TEST_ASSERT_EQUAL_STRING("100001", str(j, "pin"));
  cJSON_Delete(j);
  j = call_at("POST", path(id, "pin"), "{\"renew\":false}", tok1, 200, NULL, T + 2 * H);
  TEST_ASSERT_EQUAL_STRING("100001", str(j, "pin"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, th_pin_count());
  call_free("POST", path(id, "pin"), "{\"renew\":1}", tok1, 400, "invalid", T + 2 * H);
}

void test_pin_post_overdue(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H);
  cJSON *j = call_at("POST", path(id, "pin"), "{\"renew\":false}", tok1, 200, NULL, T + 10 * H + 600);
  TEST_ASSERT_EQUAL_STRING("100002", str(j, "pin"));
  TEST_ASSERT_EQUAL_INT64(T + 10 * H, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T + 12 * H, num(j, "valid_until"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(2, th_pin_count());
  TEST_ASSERT_EQUAL_INT64(T + 10 * H, th_pin_last_from());
  TEST_ASSERT_EQUAL_INT64(T + 12 * H, th_pin_last_until());
  TEST_ASSERT_EQUAL_INT(1, th_pin_last_variance());
  j = call_at("GET", path(id, "pin"), NULL, tok1, 200, NULL, T + 10 * H + 600);
  TEST_ASSERT_EQUAL_STRING("100002", str(j, "pin"));
  cJSON_Delete(j);
}

void test_pin_post_failure_503(void) {
  int64_t id = book(tok1, car1, T + H, T + 2 * H);
  start_ok(id, T + H);
  th_pin_fail(PIN_ERR_AUTH);
  cJSON *j = call_at("POST", path(id, "pin"), "{\"renew\":false}", tok1, 503, "pin_unavailable", T + 3 * H);
  TEST_ASSERT_EQUAL_STRING(PIN_MSG, str(j, "message"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_UINT(1, a->db.n_pins);
  TEST_ASSERT_EQUAL_INT(BK_ACTIVE, db_booking(&a->db, id)->state);
}

void test_pin_renew_too_soon(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H + 1200);
  cJSON *j = call_at("POST", path(id, "pin"), "{\"renew\":true}", tok1, 429, "pin_too_soon", T + H + 1800);
  TEST_ASSERT_EQUAL_STRING("Eine neue PIN ist ab 03:40 möglich.", str(j, "message"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, th_pin_count());
  j = call_at("POST", path(id, "pin"), "{\"renew\":true}", tok1, 200, NULL, T + H + 2400);
  TEST_ASSERT_EQUAL_STRING("100002", str(j, "pin"));
  TEST_ASSERT_EQUAL_INT64(T + H, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T + 5 * H, num(j, "valid_until"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(2, th_pin_last_variance());
  j = call_at("GET", path(id, "pin"), NULL, tok1, 200, NULL, T + H + 2400);
  TEST_ASSERT_EQUAL_STRING("100002", str(j, "pin"));
  cJSON_Delete(j);
}

void test_pin_too_soon_winter(void) {
  /* W = 2026-11-20 13:00 UTC = 14:00 CET; created_at W+30 -> 14:20:30 -> "14:21". */
  const int64_t W = 1795179600;
  char b[160];
  snprintf(b, sizeof b, "{\"car_id\":%lld,\"start\":%lld,\"end\":%lld}", (long long)car1,
           (long long)W, (long long)(W + 3 * H));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, W - H, tok1));
  cJSON *j = call_at("POST", "/api/bookings", b, tok1, 201, NULL, W - H);
  int64_t id = num(j, "id");
  cJSON_Delete(j);
  start_ok(id, W + 30);
  j = call_at("POST", path(id, "pin"), "{\"renew\":true}", tok1, 429, "pin_too_soon", W + 600);
  TEST_ASSERT_EQUAL_STRING("Eine neue PIN ist ab 14:21 möglich.", str(j, "message"));
  cJSON_Delete(j);
}

void test_variance_counts_failures(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  th_pin_fail(PIN_ERR_NETWORK);
  call_free("POST", path(id, "start"), NULL, tok1, 503, "pin_unavailable", T + H);
  TEST_ASSERT_EQUAL_INT(1, th_pin_last_variance());
  th_pin_fail(PIN_OK);
  start_ok(id, T + H + 60);
  TEST_ASSERT_EQUAL_INT(2, th_pin_last_variance());
  TEST_ASSERT_EQUAL_UINT(1, a->db.n_pins);
  TEST_ASSERT_EQUAL_INT(2, a->db.pins[0].variance);
}

void test_variance_exhausted(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  th_pin_fail(PIN_ERR_NETWORK);
  for (int i = 0; i < 3; i++) {
    call_free("POST", path(id, "start"), NULL, tok1, 503, "pin_unavailable", T + H + 60 * i);
    TEST_ASSERT_EQUAL_INT(i + 1, th_pin_last_variance());
  }
  TEST_ASSERT_EQUAL_INT(3, th_pin_count());
  th_pin_fail(PIN_OK);
  cJSON *j = call_at("POST", path(id, "start"), NULL, tok1, 503, "pin_unavailable", T + H + 180);
  TEST_ASSERT_EQUAL_STRING(
      "PIN für die Schlüsselbox konnte nicht abgerufen werden. Bitte ab 04:00 erneut versuchen. "
      "Klappt es weiter nicht: Notfallkontakt auf der Hilfe-Seite.",
      str(j, "message"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(3, th_pin_count());
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, db_booking(&a->db, id)->state);
  start_ok(id, T + 2 * H);
  TEST_ASSERT_EQUAL_INT(1, th_pin_last_variance());
  TEST_ASSERT_EQUAL_INT64(T + 2 * H, th_pin_last_from());
}

void test_variance_seeded_from_rows(void) {
  access_pin_t p;
  memset(&p, 0, sizeof p);
  p.car_id = car1;
  p.valid_from = T + H;
  p.valid_until = T + 2 * H;
  p.variance = 2;
  p.created_at = T;
  snprintf(p.pin_id, sizeof p.pin_id, "X");
  TEST_ASSERT_EQUAL_INT(0, db_put_pin(&a->db, &p));
  memset(a->pin_attempts, 0, sizeof a->pin_attempts);
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H + 300);
  TEST_ASSERT_EQUAL_INT(3, th_pin_last_variance());
}

void test_variance_per_car(void) {
  /* Another car in the same hour starts at variance 1. */
  int64_t car3 = add_car("Up", "D3");
  int64_t id1 = book(tok1, car1, T + H, T + 5 * H);
  int64_t id2 = book(tok1, car3, T + H, T + 5 * H);
  start_ok(id1, T + H);
  TEST_ASSERT_EQUAL_INT(1, th_pin_last_variance());
  start_ok(id2, T + H + 60);
  TEST_ASSERT_EQUAL_INT(1, th_pin_last_variance());
  TEST_ASSERT_EQUAL_STRING("D3", th_pin_last_device());
}

void test_attempts_table_evicts_smallest_hour(void) {
  /* Builder call 3: 16 entries; a full table replaces the one with the smallest hour. */
  access_pin_t row;
  pin_err_t err;
  th_pin_fail(PIN_ERR_NETWORK);
  for (int k = 0; k <= APP_PIN_ATTEMPTS; k++) {
    TEST_ASSERT_EQUAL_INT(-1, api_access_create(a, car1, "D1", 0, T + k * H, T + (k + 1) * H, "Test",
                                                T + k * H, &row, &err));
    TEST_ASSERT_EQUAL_INT(PIN_ERR_NETWORK, err);
    TEST_ASSERT_EQUAL_INT(1, th_pin_last_variance());
  }
  /* Hour T+H is kept: its second attempt. */
  TEST_ASSERT_EQUAL_INT(-1, api_access_create(a, car1, "D1", 0, T + H, T + 2 * H, "Test", T + H, &row, &err));
  TEST_ASSERT_EQUAL_INT(2, th_pin_last_variance());
  /* Test PIN (booking 0) succeeds and is stored. */
  th_pin_fail(PIN_OK);
  TEST_ASSERT_EQUAL_INT(0, api_access_create(a, car1, "D1", 0, T + H, T + 2 * H, "Test", T + H, &row, &err));
  TEST_ASSERT_EQUAL_INT(3, row.variance);
  TEST_ASSERT_EQUAL_INT64(0, row.booking_id);
  TEST_ASSERT_EQUAL_INT64(T + H, row.created_at);
  TEST_ASSERT_EQUAL_UINT(1, a->db.n_pins);
  int n = th_pin_count();
  TEST_ASSERT_EQUAL_INT(-2, api_access_create(a, car1, "D1", 0, T + H, T + 2 * H, "Test", T + H, &row, &err));
  TEST_ASSERT_EQUAL_INT(n, th_pin_count());
  /* Hour T was evicted by hour T+16H: its count starts over (and evicts hour T+H, now the smallest). */
  TEST_ASSERT_EQUAL_INT(0, api_access_create(a, car1, "D1", 0, T, T + H, "Test", T, &row, &err));
  TEST_ASSERT_EQUAL_INT(1, row.variance);
  /* Hour T+H is seeded from its stored row: still exhausted. */
  n = th_pin_count();
  TEST_ASSERT_EQUAL_INT(-2, api_access_create(a, car1, "D1", 0, T + H, T + 2 * H, "Test", T + H, &row, &err));
  TEST_ASSERT_EQUAL_INT(n, th_pin_count());
}

void test_admin_edit_car_blanks(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H);
  /* Only the end changes: the PIN stays. */
  call_free("PUT", admin_path(id), admin_body(car1, u1, T + H, T + 6 * H), tokadm, 200, NULL, T + H + 600);
  cJSON *j = call_at("GET", path(id, "pin"), NULL, tok1, 200, NULL, T + H + 600);
  TEST_ASSERT_EQUAL_STRING("100001", str(j, "pin"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_UINT(1, a->db.n_pins);
  TEST_ASSERT_EQUAL_STRING("100001", a->db.pins[0].pin);
  /* Another car: blanked. */
  call_free("PUT", admin_path(id), admin_body(car2, u1, T + H, T + 6 * H), tokadm, 200, NULL, T + H + 900);
  assert_pins_blank(id);
  call_free("GET", path(id, "pin"), NULL, tok1, 404, "not_found", T + H + 900);
}

void test_admin_edit_user_blanks(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H);
  call_free("PUT", admin_path(id), admin_body(car1, u2, T + H, T + 5 * H), tokadm, 200, NULL, T + H + 600);
  assert_pins_blank(id);
  /* The new member sees the booking but no PIN until asking for one. */
  cJSON *j = call_at("GET", path(id, "pin"), NULL, tok2, 200, NULL, T + H + 600);
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "pin")));
  cJSON_Delete(j);
  call_free("GET", path(id, "pin"), NULL, tok1, 404, "not_found", T + H + 600);
}

void test_admin_end_trip_blanks(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H);
  char b[200];
  snprintf(b, sizeof b,
           "{\"car_id\":%lld,\"user_id\":%lld,\"start\":%lld,\"end\":%lld,\"actual_start\":%lld,"
           "\"actual_end\":%lld}",
           (long long)car1, (long long)u1, (long long)(T + H), (long long)(T + 5 * H),
           (long long)(T + H), (long long)(T + 2 * H));
  call_free("PUT", admin_path(id), b, tokadm, 200, NULL, T + 2 * H);
  TEST_ASSERT_EQUAL_INT(BK_DONE, db_booking(&a->db, id)->state);
  assert_pins_blank(id);
}

void test_export_has_no_pin(void) {
  int64_t id = book(tok1, car1, T + H, T + 5 * H);
  start_ok(id, T + H);
  TEST_ASSERT_EQUAL_UINT(1, a->db.n_pins);
  TEST_ASSERT_EQUAL_STRING("100001", a->db.pins[0].pin);
  char p[96];
  snprintf(p, sizeof p, "/api/admin/users/%lld/export", (long long)u1);
  for (int round = 0; round < 2; round++) {
    char *s = raw("GET", p, tokadm, 200, T + 2 * H);
    TEST_ASSERT_NULL(strstr(s, "\"pin\""));
    TEST_ASSERT_NULL(strstr(s, "100001"));
    free(s);
    s = raw("GET", "/api/admin/bookings", tokadm, 200, T + 2 * H);
    TEST_ASSERT_NULL(strstr(s, "\"pin\""));
    TEST_ASSERT_NULL(strstr(s, "100001"));
    free(s);
    s = raw("GET", "/api/bookings", tok1, 200, T + 2 * H);
    TEST_ASSERT_NULL(strstr(s, "\"pin\""));
    TEST_ASSERT_NULL(strstr(s, "100001"));
    free(s);
    if (round == 0) call_free("POST", path(id, "return"), NULL, tok1, 200, NULL, T + 2 * H);
  }
}

void test_cars_has_lock(void) {
  cJSON *j = call_at("GET", "/api/cars", NULL, tok1, 200, NULL, T);
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  cJSON *c1 = cJSON_GetArrayItem(j, 0), *c2 = cJSON_GetArrayItem(j, 1);
  TEST_ASSERT_EQUAL_INT64(car1, num(c1, "id"));
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c1, "has_lock")));
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(c2, "has_lock")));
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(c1, "lock_device_id"));
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(c2, "lock_device_id"));
  cJSON_Delete(j);
}

static const char *car_body(const char *name, const char *lock_json) {
  static char b[256];
  snprintf(b, sizeof b, "{\"name\":\"%s\",\"plate\":\"X-%s\",\"location\":\"Hof\",\"active\":true%s}", name,
           name, lock_json);
  return b;
}

void test_car_lock_field(void) {
  cJSON *j = call_at("POST", "/api/admin/cars", car_body("Neu", ",\"lock_device_id\":\"D9\""), tokadm, 201, NULL, T);
  int64_t id = num(j, "id");
  TEST_ASSERT_EQUAL_STRING("D9", str(j, "lock_device_id"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_STRING("D9", db_car(&a->db, id)->lock_device_id);
  j = call_at("POST", "/api/admin/cars", car_body("Zwei", ",\"lock_device_id\":\"D9\""), tokadm, 409, "duplicate_lock", T);
  TEST_ASSERT_EQUAL_STRING("Diese Schlüsselbox ist bereits einem Auto zugeordnet.", str(j, "message"));
  cJSON_Delete(j);
  call_free("POST", "/api/admin/cars", car_body("Drei", ",\"lock_device_id\":\"a/b\""), tokadm, 400, "invalid", T);
  char p[64];
  snprintf(p, sizeof p, "/api/admin/cars/%lld", (long long)id);
  call_free("PUT", p, car_body("Neu2", ""), tokadm, 200, NULL, T);
  TEST_ASSERT_EQUAL_STRING("D9", db_car(&a->db, id)->lock_device_id);
  call_free("PUT", p, car_body("Neu2", ",\"lock_device_id\":\"\""), tokadm, 200, NULL, T);
  TEST_ASSERT_EQUAL_STRING("", db_car(&a->db, id)->lock_device_id);
}

void test_admin_locks_get(void) {
  lock_info_t l[2] = {{"D1", "Dörpsmobil 1"}, {"D2", ""}};
  th_locks_set(l, 2);
  char *s = raw("GET", "/api/admin/locks", tokadm, 200, T);
  TEST_ASSERT_EQUAL_STRING("[{\"device_id\":\"D1\",\"name\":\"Dörpsmobil 1\"},{\"device_id\":\"D2\",\"name\":\"\"}]", s);
  free(s);
  call_free("GET", "/api/admin/locks", NULL, tok1, 403, NULL, T);
  th_locks_fail(PIN_ERR_PAYMENT);
  cJSON *j = call_at("GET", "/api/admin/locks", NULL, tokadm, 503, "locks_unavailable", T);
  TEST_ASSERT_EQUAL_STRING("igloohome-API nicht bezahlt (Testzeitraum abgelaufen).", str(j, "message"));
  cJSON_Delete(j);
}

static const char *lock_test_path(int64_t id) {
  static char p[64];
  snprintf(p, sizeof p, "/api/admin/cars/%lld/lock-test", (long long)id);
  return p;
}

void test_lock_test_ok(void) {
  cJSON *j = call_at("POST", lock_test_path(car1), NULL, tokadm, 200, NULL, T + H + 1200);
  TEST_ASSERT_EQUAL_STRING("100001", str(j, "pin"));
  TEST_ASSERT_EQUAL_INT64(T + 3 * H, num(j, "valid_until"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_STRING("D1", th_pin_last_device());
  TEST_ASSERT_EQUAL_INT64(T + H, th_pin_last_from());
  TEST_ASSERT_EQUAL_INT64(T + 3 * H, th_pin_last_until());
  char name[32];
  snprintf(name, sizeof name, "Test Auto %lld", (long long)car1);
  TEST_ASSERT_EQUAL_STRING(name, th_pin_last_name());
  TEST_ASSERT_EQUAL_UINT(1, a->db.n_pins);
  TEST_ASSERT_EQUAL_INT64(0, a->db.pins[0].booking_id);
  TEST_ASSERT_EQUAL_STRING("", a->db.pins[0].pin);
  TEST_ASSERT_EQUAL_INT(1, a->db.pins[0].variance);
}

void test_lock_test_errors(void) {
  call_free("POST", lock_test_path(car2), NULL, tokadm, 400, "no_lock", T + H);
  th_pin_fail(PIN_ERR_TIMEZONE);
  cJSON *j = call_at("POST", lock_test_path(car1), NULL, tokadm, 503, "lock_test_failed", T + H);
  TEST_ASSERT_EQUAL_STRING("In der igloohome-App der Box eine Zeitzone (Europe/Berlin) zuweisen.", str(j, "message"));
  cJSON_Delete(j);
  th_pin_fail(PIN_OK);
  call_free("POST", lock_test_path(9999), NULL, tokadm, 404, "not_found", T + H);
  call_free("POST", lock_test_path(car1), NULL, tok1, 403, NULL, T + H);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_start_creates_pin);
  RUN_TEST(test_start_without_lock_unchanged);
  RUN_TEST(test_start_pin_failure_keeps_reserved);
  RUN_TEST(test_start_malformed_pin_keeps_reserved);
  RUN_TEST(test_failure_log_line);
  RUN_TEST(test_pin_get_foreign_404);
  RUN_TEST(test_pin_get_after_return_404);
  RUN_TEST(test_pin_get_expired_null);
  RUN_TEST(test_pin_get_no_rows_zeros);
  RUN_TEST(test_pin_post_usable_returns_same);
  RUN_TEST(test_pin_post_overdue);
  RUN_TEST(test_pin_post_failure_503);
  RUN_TEST(test_pin_renew_too_soon);
  RUN_TEST(test_pin_too_soon_winter);
  RUN_TEST(test_variance_counts_failures);
  RUN_TEST(test_variance_exhausted);
  RUN_TEST(test_variance_seeded_from_rows);
  RUN_TEST(test_variance_per_car);
  RUN_TEST(test_attempts_table_evicts_smallest_hour);
  RUN_TEST(test_admin_edit_car_blanks);
  RUN_TEST(test_admin_edit_user_blanks);
  RUN_TEST(test_admin_end_trip_blanks);
  RUN_TEST(test_export_has_no_pin);
  RUN_TEST(test_cars_has_lock);
  RUN_TEST(test_car_lock_field);
  RUN_TEST(test_admin_locks_get);
  RUN_TEST(test_lock_test_ok);
  RUN_TEST(test_lock_test_errors);
  return UNITY_END();
}
