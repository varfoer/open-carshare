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
  adm_free("POST", "/api/admin/tariffs", "{}", 400, "invalid");
  adm_free("POST", "/api/admin/tariffs", "{\"name\":5}", 400, "invalid");
  adm_free("POST", "/api/admin/tariffs", NULL, 400, "invalid");
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_tariffs);
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
  TEST_ASSERT_TRUE(th_assign_tariff(a, u1, s, T + DAY) > 0);
  adm_err("DELETE", tp(s), NULL, 409, "tariff_in_use", "Dieser Tarif ist Nutzern zugeordnet.");
  TEST_ASSERT_NOT_NULL(db_tariff(&a->db, s));
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_versions);
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
  return UNITY_END();
}
