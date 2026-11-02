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
static int64_t adm, u1, car1;
static char tadm[65], tu1[65];

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  adm = th_add_user(a, "admin@x.de", NULL, ROLE_ADMIN);
  u1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(adm > 0 && u1 > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tadm));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, T, tu1));
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "Golf");
  snprintf(c.plate, sizeof c.plate, "X-Golf");
  snprintf(c.location, sizeof c.location, "Hof");
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  car1 = c.id;
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

static int64_t num(const cJSON *o, const char *k) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsNumber(v));
  return (int64_t)v->valuedouble;
}

static const char *tpath(void) {
  static char p[64];
  snprintf(p, sizeof p, "/api/admin/cars/%lld/tariffs", (long long)car1);
  return p;
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

/* Posts a valid tariff; returns its id. */
static int64_t post_ok(int64_t valid_from) {
  cJSON *j = call("POST", tpath(), tbody(valid_from, 360), tadm, 201, NULL);
  int64_t id = num(j, "id");
  TEST_ASSERT_EQUAL_INT64(car1, num(j, "car_id"));
  TEST_ASSERT_EQUAL_INT64(valid_from, num(j, "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T, num(j, "created_at"));
  cJSON_Delete(j);
  return id;
}

void test_first_tariff_may_be_backdated(void) {
  post_ok(T - 100 * DAY);
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_tariffs);
}

void test_later_tariff_in_past_409(void) {
  post_ok(T - 100 * DAY);
  api_res_t r = th_req(a, "POST", tpath(), tbody(T - 1, 360), tadm, T);
  TEST_ASSERT_EQUAL_INT(409, r.status);
  TEST_ASSERT_NOT_NULL(strstr(r.body, "tariff_in_past"));
  TEST_ASSERT_NOT_NULL(strstr(r.body, "Ein neuer Tarif darf erst ab heute gelten."));
  api_res_free(&r);
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_tariffs);
}

void test_later_tariff_today_ok(void) {
  post_ok(T - 100 * DAY);
  post_ok(T);
  TEST_ASSERT_EQUAL_size_t(2, a->db.n_tariffs);
}

void test_duplicate_valid_from_409(void) {
  post_ok(T - 5 * DAY);
  post_ok(T + DAY);
  api_res_t r = th_req(a, "POST", tpath(), tbody(T + DAY, 360), tadm, T);
  TEST_ASSERT_EQUAL_INT(409, r.status);
  TEST_ASSERT_NOT_NULL(strstr(r.body, "duplicate_valid_from"));
  TEST_ASSERT_NOT_NULL(strstr(r.body, "Für diesen Zeitpunkt gibt es schon einen Tarif."));
  api_res_free(&r);
  /* duplicate wins over past: same valid_from as the backdated first one */
  adm_free("POST", tpath(), tbody(T - 5 * DAY, 360), 409, "duplicate_valid_from");
}

void test_tariff_invalid_400(void) {
  adm_free("POST", tpath(), tbody(T + DAY, 1440), 400, "invalid");
  adm_free("POST", tpath(), "{\"valid_from\":1}", 400, "invalid");
  adm_free("POST", tpath(), NULL, 400, "invalid");
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_tariffs);
}

void test_body_id_car_id_ignored(void) {
  const char *b =
      "{\"id\":99,\"car_id\":77,\"created_at\":5,\"valid_from\":1000,\"rate_day_cents_per_h\":300,"
      "\"rate_night_cents_per_h\":150,\"rate_km_cents\":30,\"night_start\":1200,\"night_end\":360,"
      "\"unused_factor_permille\":500,\"time_cap_cents_per_day\":3000,"
      "\"combined_cap_cents_per_day\":5000,\"late_fee_cents_per_started_h\":1500}";
  cJSON *j = call("POST", tpath(), b, tadm, 201, NULL);
  TEST_ASSERT_EQUAL_INT64(car1, num(j, "car_id"));
  TEST_ASSERT_EQUAL_INT64(T, num(j, "created_at"));
  TEST_ASSERT_NOT_EQUAL(99, num(j, "id"));
  cJSON_Delete(j);
}

void test_list_newest_first(void) {
  post_ok(T - 100 * DAY);
  post_ok(T + 2 * DAY);
  post_ok(T + DAY);
  cJSON *j = call("GET", tpath(), NULL, tadm, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsArray(j));
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT64(T + 2 * DAY, num(cJSON_GetArrayItem(j, 0), "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T + DAY, num(cJSON_GetArrayItem(j, 1), "valid_from"));
  TEST_ASSERT_EQUAL_INT64(T - 100 * DAY, num(cJSON_GetArrayItem(j, 2), "valid_from"));
  cJSON_Delete(j);
}

void test_delete_future_tariff(void) {
  post_ok(T - 100 * DAY);
  int64_t id = post_ok(T + DAY);
  char p[64];
  snprintf(p, sizeof p, "/api/admin/tariffs/%lld", (long long)id);
  cJSON *j = call("DELETE", p, NULL, tadm, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ok")));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_tariffs);
  adm_free("DELETE", p, NULL, 404, "not_found");
}

void test_delete_active_tariff_409(void) {
  int64_t id = post_ok(T);  /* valid_from == now counts as active */
  char p[64];
  snprintf(p, sizeof p, "/api/admin/tariffs/%lld", (long long)id);
  api_res_t r = th_req(a, "DELETE", p, NULL, tadm, T);
  TEST_ASSERT_EQUAL_INT(409, r.status);
  TEST_ASSERT_NOT_NULL(strstr(r.body, "tariff_active"));
  TEST_ASSERT_NOT_NULL(strstr(r.body, "Ein Tarif, der schon gilt, kann nicht gelöscht werden."));
  api_res_free(&r);
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_tariffs);
}

void test_tariff_routes_admin_only(void) {
  post_ok(T + DAY);
  char p[64];
  snprintf(p, sizeof p, "/api/admin/tariffs/%lld", (long long)a->db.tariffs[0].id);
  cJSON_Delete(call("GET", tpath(), NULL, tu1, 403, NULL));
  cJSON_Delete(call("POST", tpath(), tbody(T + 2 * DAY, 360), tu1, 403, NULL));
  cJSON_Delete(call("DELETE", p, NULL, tu1, 403, NULL));
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_tariffs);
}

void test_unknown_car_404(void) {
  adm_free("GET", "/api/admin/cars/999/tariffs", NULL, 404, "not_found");
  adm_free("POST", "/api/admin/cars/999/tariffs", tbody(T + DAY, 360), 404, "not_found");
  adm_free("POST", "/api/admin/cars/999/tariffs", "{}", 404, "not_found");
  adm_free("DELETE", "/api/admin/tariffs/999", NULL, 404, "not_found");
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_first_tariff_may_be_backdated);
  RUN_TEST(test_later_tariff_in_past_409);
  RUN_TEST(test_later_tariff_today_ok);
  RUN_TEST(test_duplicate_valid_from_409);
  RUN_TEST(test_tariff_invalid_400);
  RUN_TEST(test_body_id_car_id_ignored);
  RUN_TEST(test_list_newest_first);
  RUN_TEST(test_delete_future_tariff);
  RUN_TEST(test_delete_active_tariff_409);
  RUN_TEST(test_tariff_routes_admin_only);
  RUN_TEST(test_unknown_car_404);
  return UNITY_END();
}
