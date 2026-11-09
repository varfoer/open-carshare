#include <stdio.h>
#include <string.h>

#include "auth/session.h"
#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400
#define DAY 86400

static app_t *a;
static int64_t adm, u1;
static char tadm[65], tu1[65];

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  adm = th_add_user(a, "admin@x.de", NULL, ROLE_ADMIN);
  u1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(adm > 0 && u1 > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tadm));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, T, tu1));
}

void tearDown(void) { th_free(a); }

static cJSON *call(const char *m, const char *p, const char *body, const char *tok, int status,
                   const char *code) {
  api_res_t r = th_req(a, m, p, body, tok, T);
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

static void adm_free(const char *m, const char *p, const char *body, int status, const char *code) {
  cJSON_Delete(call(m, p, body, tadm, status, code));
}

/* Asserts status, error code and German message. */
static void adm_err(const char *m, const char *p, const char *body, int status, const char *code,
                    const char *msg) {
  cJSON *j = call(m, p, body, tadm, status, code);
  cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "message");
  bool ok = cJSON_IsString(e) && strcmp(e->valuestring, msg) == 0;
  cJSON_Delete(j);
  TEST_ASSERT_TRUE_MESSAGE(ok, msg);
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

static char *tbody(int64_t valid_from, int night_end) {
  static char b[512];
  snprintf(b, sizeof b,
           "{\"valid_from\":%lld,\"rate_day_cents_per_h\":300,\"rate_night_cents_per_h\":150,"
           "\"rate_km_cents\":30,\"night_start\":1200,\"night_end\":%d,"
           "\"unused_factor_permille\":500,\"time_cap_cents_per_day\":3000,"
           "\"combined_cap_cents_per_day\":5000,\"late_fee_cents_per_started_h\":1500}",
           (long long)valid_from, night_end);
  return b;
}

static const char *nbody(const char *name) {
  static char b[256];
  snprintf(b, sizeof b, "{\"name\":\"%s\"}", name);
  return b;
}

static const char *tp(int64_t tid) {
  static char p[64];
  snprintf(p, sizeof p, "/api/admin/tariffs/%lld", (long long)tid);
  return p;
}

static const char *vpath(int64_t tid) {
  static char p[64];
  snprintf(p, sizeof p, "/api/admin/tariffs/%lld/versions", (long long)tid);
  return p;
}

static const char *vdel(int64_t vid) {
  static char p[64];
  snprintf(p, sizeof p, "/api/admin/tariff-versions/%lld", (long long)vid);
  return p;
}

static void assert_empty_versions(const cJSON *o) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "versions");
  TEST_ASSERT_TRUE(cJSON_IsArray(v));
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(v));
}

/* POST /api/admin/tariffs {"name":…} -> 201; returns id. */
static int64_t post_tariff(const char *name) {
  cJSON *j = call("POST", "/api/admin/tariffs", nbody(name), tadm, 201, NULL);
  int64_t id = num(j, "id");
  TEST_ASSERT_EQUAL_INT64(T, num(j, "created_at"));
  assert_empty_versions(j);
  cJSON_Delete(j);
  return id;
}

/* Posts a valid version; returns its id. */
static int64_t post_version(int64_t tid, int64_t valid_from) {
  cJSON *j = call("POST", vpath(tid), tbody(valid_from, 360), tadm, 201, NULL);
  int64_t id = num(j, "id");
  TEST_ASSERT_EQUAL_INT64(tid, num(j, "tariff_id"));
  TEST_ASSERT_EQUAL_INT64(valid_from, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T, num(j, "created_at"));
  TEST_ASSERT_EQUAL_INT64(300, num(j, "rate_day_cents_per_h"));
  cJSON_Delete(j);
  return id;
}

void test_create_and_list_tariffs(void) {
  int64_t s = post_tariff("Standard");
  int64_t r = post_tariff("Reduziert");
  TEST_ASSERT_TRUE(s > 0 && r > s);
  cJSON *j = call("GET", "/api/admin/tariffs", NULL, tadm, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsArray(j));
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  cJSON *t0 = cJSON_GetArrayItem(j, 0), *t1 = cJSON_GetArrayItem(j, 1);
  TEST_ASSERT_EQUAL_INT64(s, num(t0, "id"));
  TEST_ASSERT_EQUAL_INT64(r, num(t1, "id"));
  TEST_ASSERT_EQUAL_STRING("Standard", str(t0, "name"));
  TEST_ASSERT_EQUAL_STRING("Reduziert", str(t1, "name"));
  assert_empty_versions(t0);
  assert_empty_versions(t1);
  cJSON_Delete(j);
}

void test_name_trimmed_and_unique(void) {
  cJSON *j = call("POST", "/api/admin/tariffs", nbody("  Standard "), tadm, 201, NULL);
  TEST_ASSERT_EQUAL_STRING("Standard", str(j, "name"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_STRING("Standard", a->db.tariffs[0].name);
  adm_err("POST", "/api/admin/tariffs", nbody("Standard"), 409, "duplicate_name",
          "Diesen Tarifnamen gibt es schon.");
  adm_err("POST", "/api/admin/tariffs", nbody(" Standard  "), 409, "duplicate_name",
          "Diesen Tarifnamen gibt es schon.");
  adm_free("POST", "/api/admin/tariffs", nbody("   "), 400, "invalid");
  adm_free("POST", "/api/admin/tariffs", nbody(""), 400, "invalid");
  char n64[65];
  memset(n64, 'a', 64);
  n64[64] = 0;
  adm_free("POST", "/api/admin/tariffs", nbody(n64), 400, "invalid");
  /* length is checked after trimming: 63 bytes plus padding fits, 64 plus padding does not */
  char padded[80];
  snprintf(padded, sizeof padded, "   %.63s  ", n64);
  j = call("POST", "/api/admin/tariffs", nbody(padded), tadm, 201, NULL);
  TEST_ASSERT_EQUAL_STRING(n64 + 1, str(j, "name"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_size_t(63, strlen(a->db.tariffs[1].name));
  snprintf(padded, sizeof padded, "   %s  ", n64);
  adm_free("POST", "/api/admin/tariffs", nbody(padded), 400, "invalid");
  adm_free("POST", "/api/admin/tariffs", "{}", 400, "invalid");
  adm_free("POST", "/api/admin/tariffs", "{\"name\":5}", 400, "invalid");
  adm_free("POST", "/api/admin/tariffs", NULL, 400, "invalid");
  TEST_ASSERT_EQUAL_size_t(2, a->db.n_tariffs);
}

void test_rename(void) {
  int64_t s = post_tariff("Standard");
  post_tariff("Reduziert");
  cJSON *j = call("PUT", tp(s), nbody(" Normal "), tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(s, num(j, "id"));
  TEST_ASSERT_EQUAL_STRING("Normal", str(j, "name"));
  TEST_ASSERT_EQUAL_INT64(T, num(j, "created_at"));
  assert_empty_versions(j);
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_STRING("Normal", db_tariff(&a->db, s)->name);
  adm_err("PUT", tp(s), nbody("Reduziert"), 409, "duplicate_name",
          "Diesen Tarifnamen gibt es schon.");
  TEST_ASSERT_EQUAL_STRING("Normal", db_tariff(&a->db, s)->name);
  j = call("PUT", tp(s), nbody("Normal"), tadm, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("Normal", str(j, "name"));
  cJSON_Delete(j);
  adm_free("PUT", tp(s), nbody("  "), 400, "invalid");
  adm_free("PUT", tp(s), "{}", 400, "invalid");
  adm_free("PUT", tp(999), nbody("X"), 404, "not_found");
  TEST_ASSERT_EQUAL_size_t(2, a->db.n_tariffs);
}

void test_delete_unassigned_removes_versions(void) {
  int64_t s = post_tariff("Standard");
  post_version(s, T - DAY);
  post_version(s, T + DAY);
  TEST_ASSERT_EQUAL_size_t(2, a->db.n_versions);
  cJSON *j = call("DELETE", tp(s), NULL, tadm, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok")));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_tariffs);
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_versions);
  adm_free("DELETE", tp(s), NULL, 404, "not_found");
}

void test_delete_assigned_409(void) {
  int64_t s = post_tariff("Standard");
  post_version(s, T - DAY);
  int64_t asg = th_assign_tariff(a, u1, s, T + DAY);
  TEST_ASSERT_TRUE(asg > 0);
  adm_err("DELETE", tp(s), NULL, 409, "tariff_in_use", "Dieser Tarif ist Nutzern zugeordnet.");
  TEST_ASSERT_NOT_NULL(db_tariff(&a->db, s));
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_versions);
  /* deleting that future assignment frees the tariff */
  char up[64];
  snprintf(up, sizeof up, "/api/admin/user-tariffs/%lld", (long long)asg);
  adm_free("DELETE", up, NULL, 200, NULL);
  adm_free("DELETE", tp(s), NULL, 200, NULL);
  TEST_ASSERT_NULL(db_tariff(&a->db, s));
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_versions);
}

void test_first_version_may_be_backdated(void) {
  int64_t s = post_tariff("Standard");
  post_version(s, T - 100 * DAY);
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_versions);
}

void test_later_version_in_past_409(void) {
  int64_t s = post_tariff("Standard");
  post_version(s, T - 100 * DAY);
  adm_err("POST", vpath(s), tbody(T - 1, 360), 409, "tariff_in_past",
          "Ein neuer Tarif darf erst ab heute gelten.");
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_versions);
  /* another tariff's versions do not count: its first version may be backdated */
  int64_t r = post_tariff("Reduziert");
  post_version(r, T - 1);
  TEST_ASSERT_EQUAL_size_t(2, a->db.n_versions);
}

void test_later_version_today_ok(void) {
  int64_t s = post_tariff("Standard");
  post_version(s, T - 100 * DAY);
  post_version(s, T);
  TEST_ASSERT_EQUAL_size_t(2, a->db.n_versions);
}

void test_version_duplicate_valid_from_409(void) {
  int64_t s = post_tariff("Standard");
  post_version(s, T - 5 * DAY);
  post_version(s, T + DAY);
  adm_err("POST", vpath(s), tbody(T + DAY, 360), 409, "duplicate_valid_from",
          "Für diesen Zeitpunkt gibt es schon einen Tarif.");
  /* duplicate wins over past: same valid_from as the backdated first one */
  adm_free("POST", vpath(s), tbody(T - 5 * DAY, 360), 409, "duplicate_valid_from");
  TEST_ASSERT_EQUAL_size_t(2, a->db.n_versions);
  /* same valid_from on another tariff is fine */
  int64_t r = post_tariff("Reduziert");
  post_version(r, T + DAY);
  TEST_ASSERT_EQUAL_size_t(3, a->db.n_versions);
}

void test_version_invalid_400(void) {
  int64_t s = post_tariff("Standard");
  adm_free("POST", vpath(s), tbody(T + DAY, 1440), 400, "invalid");
  adm_free("POST", vpath(s), "{\"valid_from\":1}", 400, "invalid");
  adm_free("POST", vpath(s), NULL, 400, "invalid");
  adm_free("POST", vpath(s), tbody(-1, 360), 400, "invalid");
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_versions);
}

void test_version_body_ids_ignored(void) {
  int64_t s = post_tariff("Standard");
  const char *b =
      "{\"id\":99,\"tariff_id\":77,\"created_at\":5,\"valid_from\":1000,\"rate_day_cents_per_h\":300,"
      "\"rate_night_cents_per_h\":150,\"rate_km_cents\":30,\"night_start\":1200,\"night_end\":360,"
      "\"unused_factor_permille\":500,\"time_cap_cents_per_day\":3000,"
      "\"combined_cap_cents_per_day\":5000,\"late_fee_cents_per_started_h\":1500}";
  cJSON *j = call("POST", vpath(s), b, tadm, 201, NULL);
  TEST_ASSERT_EQUAL_INT64(s, num(j, "tariff_id"));
  TEST_ASSERT_EQUAL_INT64(T, num(j, "created_at"));
  TEST_ASSERT_NOT_EQUAL(99, num(j, "id"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT64(s, a->db.versions[0].tariff_id);
}

void test_versions_listed_newest_first(void) {
  int64_t s = post_tariff("Standard");
  post_version(s, T - 100 * DAY);
  post_version(s, T + 2 * DAY);
  post_version(s, T + DAY);
  int64_t r = post_tariff("Reduziert");
  post_version(r, T + 3 * DAY);
  cJSON *j = call("GET", "/api/admin/tariffs", NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  cJSON *v = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(j, 0), "versions");
  TEST_ASSERT_TRUE(cJSON_IsArray(v));
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(v));
  TEST_ASSERT_EQUAL_INT64(T + 2 * DAY, num(cJSON_GetArrayItem(v, 0), "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T + DAY, num(cJSON_GetArrayItem(v, 1), "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T - 100 * DAY, num(cJSON_GetArrayItem(v, 2), "valid_from"));
  TEST_ASSERT_EQUAL_INT64(s, num(cJSON_GetArrayItem(v, 0), "tariff_id"));
  v = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(j, 1), "versions");
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(v));
  TEST_ASSERT_EQUAL_INT64(T + 3 * DAY, num(cJSON_GetArrayItem(v, 0), "valid_from"));
  cJSON_Delete(j);
}

void test_delete_future_version(void) {
  int64_t s = post_tariff("Standard");
  post_version(s, T - 100 * DAY);
  int64_t id = post_version(s, T + DAY);
  cJSON *j = call("DELETE", vdel(id), NULL, tadm, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok")));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_versions);
  TEST_ASSERT_NULL(db_version(&a->db, id));
  adm_free("DELETE", vdel(id), NULL, 404, "not_found");
}

void test_delete_active_version_409(void) {
  int64_t s = post_tariff("Standard");
  int64_t id = post_version(s, T); /* valid_from == now counts as active */
  adm_err("DELETE", vdel(id), NULL, 409, "tariff_active",
          "Ein Tarif, der schon gilt, kann nicht gelöscht werden.");
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_versions);
}

void test_unknown_tariff_404(void) {
  adm_free("POST", "/api/admin/tariffs/999/versions", tbody(T + DAY, 360), 404, "not_found");
  adm_free("POST", "/api/admin/tariffs/999/versions", "{}", 404, "not_found");
  adm_free("DELETE", "/api/admin/tariffs/999", NULL, 404, "not_found");
  adm_free("DELETE", "/api/admin/tariff-versions/999", NULL, 404, "not_found");
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_versions);
}

void test_tariff_routes_admin_only(void) {
  int64_t s = post_tariff("Standard");
  int64_t v = post_version(s, T + DAY);
  cJSON_Delete(call("GET", "/api/admin/tariffs", NULL, tu1, 403, NULL));
  cJSON_Delete(call("POST", "/api/admin/tariffs", nbody("Neu"), tu1, 403, NULL));
  cJSON_Delete(call("PUT", tp(s), nbody("Neu"), tu1, 403, NULL));
  cJSON_Delete(call("DELETE", tp(s), NULL, tu1, 403, NULL));
  cJSON_Delete(call("POST", vpath(s), tbody(T + 2 * DAY, 360), tu1, 403, NULL));
  cJSON_Delete(call("DELETE", vdel(v), NULL, tu1, 403, NULL));
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_tariffs);
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_versions);
  TEST_ASSERT_EQUAL_STRING("Standard", a->db.tariffs[0].name);
}

/* ---- user tariff assignments ---- */

static int64_t std, red, first;

/* std and red each with a version at T - 100*DAY; u1 assigned std at T - 50*DAY (first). */
static void fx_assign(void) {
  std = th_add_tariff(a, "Standard");
  red = th_add_tariff(a, "Reduziert");
  TEST_ASSERT_TRUE(std > 0 && red > 0);
  TEST_ASSERT_TRUE(th_add_version(a, std, T - 100 * DAY, 0, 0, 0) > 0);
  TEST_ASSERT_TRUE(th_add_version(a, red, T - 100 * DAY, 0, 0, 0) > 0);
  first = th_assign_tariff(a, u1, std, T - 50 * DAY);
  TEST_ASSERT_TRUE(first > 0);
}

static const char *utp(int64_t uid) {
  static char p[64];
  snprintf(p, sizeof p, "/api/admin/users/%lld/tariffs", (long long)uid);
  return p;
}

static const char *ut(int64_t id) {
  static char p[64];
  snprintf(p, sizeof p, "/api/admin/user-tariffs/%lld", (long long)id);
  return p;
}

static const char *abody(int64_t tid, int64_t valid_from) {
  static char b[128];
  snprintf(b, sizeof b, "{\"tariff_id\":%lld,\"valid_from\":%lld}", (long long)tid,
           (long long)valid_from);
  return b;
}

static const char *vfbody(int64_t valid_from) {
  static char b[64];
  snprintf(b, sizeof b, "{\"valid_from\":%lld}", (long long)valid_from);
  return b;
}

static const char *tidbody(int64_t tid) {
  static char b[64];
  snprintf(b, sizeof b, "{\"tariff_id\":%lld}", (long long)tid);
  return b;
}

/* POST users/u1/tariffs -> 201; returns id. */
static int64_t post_assign(int64_t tid, int64_t valid_from) {
  cJSON *j = call("POST", utp(u1), abody(tid, valid_from), tadm, 201, NULL);
  int64_t id = num(j, "id");
  TEST_ASSERT_EQUAL_INT64(u1, num(j, "user_id"));
  TEST_ASSERT_EQUAL_INT64(tid, num(j, "tariff_id"));
  TEST_ASSERT_EQUAL_INT64(valid_from, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T, num(j, "created_at"));
  TEST_ASSERT_EQUAL_INT64(adm, num(j, "created_by"));
  cJSON_Delete(j);
  return id;
}

/* Done booking of u1 starting at start, invoiced when invoice_id != 0; returns id. */
static int64_t plant_booking(int64_t start, int64_t invoice_id) {
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "Golf");
  snprintf(c.plate, sizeof c.plate, "X-1");
  snprintf(c.location, sizeof c.location, "Hof");
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = c.id;
  b.user_id = u1;
  b.booked_start = start;
  b.booked_end = start + 3600;
  b.state = BK_DONE;
  b.actual_start = start;
  b.actual_end = start + 3600;
  b.created_at = start - DAY;
  b.invoice_id = invoice_id;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  return b.id;
}

static void set_invoice(int64_t bid, int64_t invoice_id) {
  booking_t b = *db_booking(&a->db, bid);
  b.invoice_id = invoice_id;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
}

void test_switch_future_ok_and_listed(void) {
  fx_assign();
  int64_t id = post_assign(red, T + DAY);
  TEST_ASSERT_TRUE(id > first);
  cJSON *j = call("GET", utp(u1), NULL, tadm, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsArray(j));
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT64(T + DAY, num(cJSON_GetArrayItem(j, 0), "valid_from"));
  TEST_ASSERT_EQUAL_INT64(id, num(cJSON_GetArrayItem(j, 0), "id"));
  TEST_ASSERT_EQUAL_INT64(red, num(cJSON_GetArrayItem(j, 0), "tariff_id"));
  TEST_ASSERT_EQUAL_INT64(T - 50 * DAY, num(cJSON_GetArrayItem(j, 1), "valid_from"));
  TEST_ASSERT_EQUAL_INT64(first, num(cJSON_GetArrayItem(j, 1), "id"));
  cJSON_Delete(j);
  /* another user's list is empty */
  j = call("GET", utp(adm), NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(j));
  cJSON_Delete(j);
  adm_free("GET", utp(999), NULL, 404, "not_found");
}

void test_switch_today_ok(void) {
  fx_assign();
  post_assign(red, T);
  TEST_ASSERT_EQUAL_INT64(red, db_user_tariff_at(&a->db, u1, T)->tariff_id);
}

void test_switch_in_past_409(void) {
  fx_assign();
  adm_err("POST", utp(u1), abody(red, T - 1), 409, "assignment_in_past",
          "Ein Tarifwechsel darf erst ab heute gelten.");
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_user_tariffs);
}

void test_switch_duplicate_409(void) {
  fx_assign();
  adm_err("POST", utp(u1), abody(red, T - 50 * DAY), 409, "duplicate_valid_from",
          "Für diesen Zeitpunkt gibt es schon eine Zuordnung.");
  post_assign(red, T + DAY);
  adm_free("POST", utp(u1), abody(std, T + DAY), 409, "duplicate_valid_from");
  TEST_ASSERT_EQUAL_size_t(2, a->db.n_user_tariffs);
}

void test_switch_unknown_tariff_400(void) {
  fx_assign();
  adm_free("POST", utp(u1), abody(999, T + DAY), 400, "invalid");
  adm_free("POST", utp(u1), tidbody(red), 400, "invalid");
  adm_free("POST", utp(u1), "{\"tariff_id\":1,\"valid_from\":\"x\"}", 400, "invalid");
  adm_free("POST", utp(u1), NULL, 400, "invalid");
  adm_free("POST", utp(999), abody(red, T + DAY), 404, "not_found");
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_user_tariffs);
}

void test_repair_backdates_newest(void) {
  fx_assign();
  TEST_ASSERT_NULL(db_user_tariff_at(&a->db, u1, T - 150 * DAY));
  TEST_ASSERT_NULL(db_tariff_at(&a->db, u1, T - 90 * DAY));
  cJSON *j = call("PUT", ut(first), vfbody(T - 200 * DAY), tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(first, num(j, "id"));
  TEST_ASSERT_EQUAL_INT64(T - 200 * DAY, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(std, num(j, "tariff_id"));
  TEST_ASSERT_EQUAL_INT64(u1, num(j, "user_id"));
  cJSON_Delete(j);
  j = call("GET", utp(u1), NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT64(T - 200 * DAY, num(cJSON_GetArrayItem(j, 0), "valid_from"));
  cJSON_Delete(j);
  /* assignment from T-200d now covers T-150d; std's prices exist only from T-100d */
  TEST_ASSERT_EQUAL_INT64(first, db_user_tariff_at(&a->db, u1, T - 150 * DAY)->id);
  TEST_ASSERT_NULL(db_tariff_at(&a->db, u1, T - 150 * DAY));
  TEST_ASSERT_NOT_NULL(db_tariff_at(&a->db, u1, T - 90 * DAY));
}

void test_repair_changes_tariff_only(void) {
  fx_assign();
  cJSON *j = call("PUT", ut(first), tidbody(red), tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(red, num(j, "tariff_id"));
  TEST_ASSERT_EQUAL_INT64(T - 50 * DAY, num(j, "valid_from"));
  cJSON_Delete(j);
  const user_tariff_t *u = db_user_tariff(&a->db, first);
  TEST_ASSERT_EQUAL_INT64(red, u->tariff_id);
  TEST_ASSERT_EQUAL_INT64(T - 50 * DAY, u->valid_from);
}

void test_repair_rejects_older(void) {
  fx_assign();
  post_assign(red, T + DAY);
  adm_err("PUT", ut(first), tidbody(red), 409, "assignment_not_newest",
          "Nur die neueste Tarifzuordnung kann geändert werden.");
  /* not newest wins over an invalid body */
  adm_free("PUT", ut(first), "{}", 409, "assignment_not_newest");
  TEST_ASSERT_EQUAL_INT64(std, db_user_tariff(&a->db, first)->tariff_id);
  adm_free("PUT", ut(999), tidbody(red), 404, "not_found");
}

void test_repair_valid_from_vs_previous(void) {
  fx_assign();
  int64_t nw = post_assign(red, T + DAY);
  adm_err("PUT", ut(nw), vfbody(T - 50 * DAY), 409, "duplicate_valid_from",
          "Für diesen Zeitpunkt gibt es schon eine Zuordnung.");
  adm_free("PUT", ut(nw), vfbody(T - 60 * DAY), 400, "invalid");
  TEST_ASSERT_EQUAL_INT64(T + DAY, db_user_tariff(&a->db, nw)->valid_from);
  cJSON *j = call("PUT", ut(nw), vfbody(T - 49 * DAY), tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(T - 49 * DAY, num(j, "valid_from"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT64(T - 49 * DAY, db_user_tariff(&a->db, nw)->valid_from);
}

void test_repair_empty_body_400(void) {
  fx_assign();
  adm_free("PUT", ut(first), "{}", 400, "invalid");
  adm_free("PUT", ut(first), NULL, 400, "invalid");
  adm_free("PUT", ut(first), tidbody(999), 400, "invalid");
  adm_free("PUT", ut(first), "{\"valid_from\":\"x\"}", 400, "invalid");
  const user_tariff_t *u = db_user_tariff(&a->db, first);
  TEST_ASSERT_EQUAL_INT64(std, u->tariff_id);
  TEST_ASSERT_EQUAL_INT64(T - 50 * DAY, u->valid_from);
}

void test_repair_billed_409(void) {
  fx_assign();
  int64_t bid = plant_booking(T - 10 * DAY, 1);
  adm_err("PUT", ut(first), vfbody(T - 20 * DAY), 409, "assignment_billed",
          "Es gibt schon abgerechnete Fahrten in diesem Zeitraum.");
  /* tariff only: valid_from stays T-50d <= T-10d (Review Focus 2) */
  adm_err("PUT", ut(first), tidbody(red), 409, "assignment_billed",
          "Es gibt schon abgerechnete Fahrten in diesem Zeitraum.");
  /* moving past the billed booking is caught by the old valid_from */
  adm_free("PUT", ut(first), vfbody(T + DAY), 409, "assignment_billed");
  const user_tariff_t *u = db_user_tariff(&a->db, first);
  TEST_ASSERT_EQUAL_INT64(std, u->tariff_id);
  TEST_ASSERT_EQUAL_INT64(T - 50 * DAY, u->valid_from);
  set_invoice(bid, 0);
  cJSON *j = call("PUT", ut(first), tidbody(red), tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(red, num(j, "tariff_id"));
  cJSON_Delete(j);
}

void test_delete_newest_and_only(void) {
  fx_assign();
  TEST_ASSERT_NOT_NULL(db_tariff_at(&a->db, u1, T));
  cJSON *j = call("DELETE", ut(first), NULL, tadm, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok")));
  cJSON_Delete(j);
  j = call("GET", utp(u1), NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(j));
  cJSON_Delete(j);
  TEST_ASSERT_NULL(db_tariff_at(&a->db, u1, T));
  adm_free("DELETE", ut(first), NULL, 404, "not_found");
}

void test_delete_rejects_older_and_billed(void) {
  fx_assign();
  int64_t second = post_assign(red, T + DAY);
  adm_err("DELETE", ut(first), NULL, 409, "assignment_not_newest",
          "Nur die neueste Tarifzuordnung kann geändert werden.");
  adm_free("DELETE", ut(second), NULL, 200, NULL);
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_user_tariffs);
  plant_booking(T - 10 * DAY, 1);
  adm_err("DELETE", ut(first), NULL, 409, "assignment_billed",
          "Es gibt schon abgerechnete Fahrten in diesem Zeitraum.");
  TEST_ASSERT_NOT_NULL(db_user_tariff(&a->db, first));
}

void test_assignment_routes_admin_only(void) {
  fx_assign();
  cJSON_Delete(call("GET", utp(u1), NULL, tu1, 403, NULL));
  cJSON_Delete(call("POST", utp(u1), abody(red, T + DAY), tu1, 403, NULL));
  cJSON_Delete(call("PUT", ut(first), tidbody(red), tu1, 403, NULL));
  cJSON_Delete(call("DELETE", ut(first), NULL, tu1, 403, NULL));
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_user_tariffs);
  TEST_ASSERT_EQUAL_INT64(std, db_user_tariff(&a->db, first)->tariff_id);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_create_and_list_tariffs);
  RUN_TEST(test_name_trimmed_and_unique);
  RUN_TEST(test_rename);
  RUN_TEST(test_delete_unassigned_removes_versions);
  RUN_TEST(test_delete_assigned_409);
  RUN_TEST(test_first_version_may_be_backdated);
  RUN_TEST(test_later_version_in_past_409);
  RUN_TEST(test_later_version_today_ok);
  RUN_TEST(test_version_duplicate_valid_from_409);
  RUN_TEST(test_version_invalid_400);
  RUN_TEST(test_version_body_ids_ignored);
  RUN_TEST(test_versions_listed_newest_first);
  RUN_TEST(test_delete_future_version);
  RUN_TEST(test_delete_active_version_409);
  RUN_TEST(test_unknown_tariff_404);
  RUN_TEST(test_tariff_routes_admin_only);
  RUN_TEST(test_switch_future_ok_and_listed);
  RUN_TEST(test_switch_today_ok);
  RUN_TEST(test_switch_in_past_409);
  RUN_TEST(test_switch_duplicate_409);
  RUN_TEST(test_switch_unknown_tariff_400);
  RUN_TEST(test_repair_backdates_newest);
  RUN_TEST(test_repair_changes_tariff_only);
  RUN_TEST(test_repair_rejects_older);
  RUN_TEST(test_repair_valid_from_vs_previous);
  RUN_TEST(test_repair_empty_body_400);
  RUN_TEST(test_repair_billed_409);
  RUN_TEST(test_delete_newest_and_only);
  RUN_TEST(test_delete_rejects_older_and_billed);
  RUN_TEST(test_assignment_routes_admin_only);
  return UNITY_END();
}
