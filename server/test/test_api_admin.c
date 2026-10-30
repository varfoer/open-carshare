#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/session.h"
#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400
#define H 3600

static app_t *a;
static int64_t adm, u1, car1, car2;
static char tadm[65], tu1[65];

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
  adm = th_add_user(a, "admin@x.de", NULL, ROLE_ADMIN);
  u1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(adm > 0 && u1 > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tadm));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, T, tu1));
  car1 = add_car("Golf", true);
  car2 = add_car("Polo", true);
  settings_t s = {15, 60, 500};
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&a->db, &s));
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

static void cfree(const char *m, const char *p, const char *body, const char *tok, int status,
                  const char *code) {
  cJSON_Delete(call(m, p, body, tok, status, code));
}

static void adm_free(const char *m, const char *p, const char *body, int status, const char *code) {
  cJSON_Delete(call(m, p, body, tadm, status, code));
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

static char *user_body(const char *email, const char *role) {
  static char b[400];
  snprintf(b, sizeof b,
           "{\"email\":\"%s\",\"name\":\"Neu Nutzer\",\"street\":\"Weg 1\",\"zip\":\"12345\","
           "\"city\":\"Ort\",\"role\":\"%s\"}",
           email, role);
  return b;
}

static char *user_put_body(const char *email, const char *role, bool active) {
  static char b[400];
  snprintf(b, sizeof b,
           "{\"email\":\"%s\",\"name\":\"Anna\",\"street\":\"S\",\"zip\":\"1\",\"city\":\"C\","
           "\"role\":\"%s\",\"active\":%s}",
           email, role, active ? "true" : "false");
  return b;
}

static char *bk_body(int64_t car, int64_t user, int64_t s, int64_t e) {
  static char b[200];
  snprintf(b, sizeof b, "{\"car_id\":%lld,\"user_id\":%lld,\"start\":%lld,\"end\":%lld}",
           (long long)car, (long long)user, (long long)s, (long long)e);
  return b;
}

static int64_t mk_booking(int64_t car, int64_t user, int64_t s, int64_t e) {
  cJSON *j = call("POST", "/api/admin/bookings", bk_body(car, user, s, e), tadm, 201, NULL);
  int64_t id = num(j, "id");
  cJSON_Delete(j);
  return id;
}

void test_user_role_gets_403_on_admin(void) {
  cfree("GET", "/api/admin/users", NULL, tu1, 403, "forbidden");
  cfree("GET", "/api/admin/cars", NULL, tu1, 403, "forbidden");
  cfree("GET", "/api/admin/bookings", NULL, tu1, 403, "forbidden");
  cfree("PUT", "/api/admin/settings", "{}", tu1, 403, "forbidden");
  cfree("POST", "/api/admin/users/1/invite", NULL, tu1, 403, "forbidden");
  cfree("GET", "/api/admin/users", NULL, NULL, 401, "unauthenticated");
}

void test_admin_create_user_sends_invite(void) {
  cJSON *j = call("POST", "/api/admin/users", user_body("  Neu@X.de ", "user"), tadm, 201, NULL);
  cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "user");
  TEST_ASSERT_NOT_NULL(u);
  TEST_ASSERT_EQUAL_STRING("neu@x.de", str(u, "email"));
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(u, "pw_hash"));
  int64_t id = num(u, "id");
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("neu@x.de", th_last_mail_to());
  TEST_ASSERT_EQUAL_STRING("Einladung zu open-carshare", th_last_mail_subject());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "/#/passwort?token="));
  user_t *s = db_user(&a->db, id);
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_EQUAL_STRING("", s->pw_hash);
  TEST_ASSERT_TRUE(s->active);
  TEST_ASSERT_EQUAL_INT64(T, s->created_at);
  TEST_ASSERT_EQUAL_INT(ROLE_USER, s->role);
}

void test_admin_create_user_validation(void) {
  adm_free("POST", "/api/admin/users", user_body("a@@x.de", "user"), 400, "invalid_email");
  adm_free("POST", "/api/admin/users", user_body("nodomain", "user"), 400, "invalid_email");
  adm_free("POST", "/api/admin/users", user_body("a@b@c", "user"), 400, "invalid_email");
  adm_free("POST", "/api/admin/users", user_body("ok@x.de", "root"), 400, "invalid");
  adm_free("POST", "/api/admin/users", "{\"email\":\"ok@x.de\"}", 400, "invalid");
  adm_free("POST", "/api/admin/users", NULL, 400, "invalid");
  char big[300];
  memset(big, 'a', sizeof big - 1);
  big[sizeof big - 1] = 0;
  char b[900];
  snprintf(b, sizeof b, "{\"email\":\"ok@x.de\",\"name\":\"%s\",\"street\":\"\",\"zip\":\"\","
                        "\"city\":\"\",\"role\":\"user\"}", big);
  adm_free("POST", "/api/admin/users", b, 400, "invalid");
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
  adm_free("POST", "/api/admin/users", user_body("ok@x.de", "admin"), 201, NULL);
  TEST_ASSERT_EQUAL_INT(ROLE_ADMIN, db_user_by_email(&a->db, "ok@x.de")->role);
}

void test_admin_create_duplicate_email_409(void) {
  adm_free("POST", "/api/admin/users", user_body("Anna2@x.de", "user"), 201, NULL);
  adm_free("POST", "/api/admin/users", user_body("anna2@x.de", "user"), 409, "duplicate_email");
  adm_free("POST", "/api/admin/users", user_body("ANNA@x.de", "user"), 409, "duplicate_email");
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
}

void test_admin_create_user_mail_failure(void) {
  th_mail_fail(1);
  cJSON *j = call("POST", "/api/admin/users", user_body("m@x.de", "user"), tadm, 201, NULL);
  cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "mail_sent");
  TEST_ASSERT_TRUE(cJSON_IsFalse(m));
  cJSON_Delete(j);
  TEST_ASSERT_NOT_NULL(db_user_by_email(&a->db, "m@x.de"));
  th_mail_fail(0);
}

void test_invited_user_cannot_login_before_set(void) {
  adm_free("POST", "/api/admin/users", user_body("neu@x.de", "user"), 201, NULL);
  cfree("POST", "/api/login", "{\"email\":\"neu@x.de\",\"password\":\"\"}", NULL, 401, NULL);
  cfree("POST", "/api/login", "{\"email\":\"neu@x.de\",\"password\":\"irgendwas12345\"}", NULL, 401,
        NULL);
}

void test_admin_users_list_public(void) {
  cJSON *j = call("GET", "/api/admin/users", NULL, tadm, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsArray(j));
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(j, 0), "pw_hash"));
  cJSON_Delete(j);
}

void test_admin_user_put_updates(void) {
  char p[64];
  snprintf(p, sizeof p, "/api/admin/users/%lld", (long long)u1);
  cJSON *j = call("PUT", p, user_put_body("Anna.Neu@X.de", "admin", true), tadm, 200, NULL);
  cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "user");
  TEST_ASSERT_EQUAL_STRING("anna.neu@x.de", str(u, "email"));
  TEST_ASSERT_EQUAL_STRING("admin", str(u, "role"));
  cJSON_Delete(j);
  user_t *s = db_user(&a->db, u1);
  TEST_ASSERT_EQUAL_STRING("Anna", s->name);
  TEST_ASSERT_EQUAL_INT(ROLE_ADMIN, s->role);
  /* same email, own case: not a duplicate */
  adm_free("PUT", p, user_put_body("ANNA.neu@x.de", "admin", true), 200, NULL);
  adm_free("PUT", "/api/admin/users/9999", user_put_body("q@x.de", "user", true), 404, "not_found");
  adm_free("PUT", "/api/admin/users/abc", user_put_body("q@x.de", "user", true), 400, "invalid");
  adm_free("PUT", p, user_put_body("q@x.de", "boss", true), 400, "invalid");
  adm_free("PUT", p, "{\"email\":\"q@x.de\"}", 400, "invalid");
}

void test_admin_user_put_duplicate_email_409(void) {
  char p[64];
  snprintf(p, sizeof p, "/api/admin/users/%lld", (long long)u1);
  adm_free("PUT", p, user_put_body("ADMIN@x.de", "user", true), 409, "duplicate_email");
  TEST_ASSERT_EQUAL_STRING("anna@x.de", db_user(&a->db, u1)->email);
}

void test_deactivate_user_kills_session(void) {
  cfree("GET", "/api/me", NULL, tu1, 200, NULL);
  char p[64];
  snprintf(p, sizeof p, "/api/admin/users/%lld", (long long)u1);
  cJSON *j = call("PUT", p, user_put_body("anna@x.de", "user", false), tadm, 200, NULL);
  cJSON_Delete(j);
  cfree("GET", "/api/me", NULL, tu1, 401, NULL);
  TEST_ASSERT_FALSE(db_user(&a->db, u1)->active);
  for (size_t i = 0; i < a->db.n_sessions; i++)
    TEST_ASSERT_TRUE(a->db.sessions[i].user_id != u1);
  /* reactivation keeps working */
  adm_free("PUT", p, user_put_body("anna@x.de", "user", true), 200, NULL);
  TEST_ASSERT_TRUE(db_user(&a->db, u1)->active);
}

void test_admin_cannot_deactivate_self(void) {
  char p[64];
  snprintf(p, sizeof p, "/api/admin/users/%lld", (long long)adm);
  adm_free("PUT", p, user_put_body("admin@x.de", "admin", false), 400, "self");
  adm_free("PUT", p, user_put_body("admin@x.de", "user", true), 400, "self");
  TEST_ASSERT_TRUE(db_user(&a->db, adm)->active);
  TEST_ASSERT_EQUAL_INT(ROLE_ADMIN, db_user(&a->db, adm)->role);
  adm_free("PUT", p, user_put_body("admin@x.de", "admin", true), 200, NULL);
}

void test_admin_invite_resend(void) {
  char p[64];
  snprintf(p, sizeof p, "/api/admin/users/%lld/invite", (long long)u1);
  adm_free("POST", p, NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("anna@x.de", th_last_mail_to());
  TEST_ASSERT_EQUAL_STRING("Einladung zu open-carshare", th_last_mail_subject());
  adm_free("POST", "/api/admin/users/9999/invite", NULL, 404, "not_found");
  adm_free("POST", "/api/admin/users/x1/invite", NULL, 400, "invalid");
  adm_free("POST", "/api/admin/users//invite", NULL, 404, "not_found");
}

void test_admin_cars_crud(void) {
  const char *b = "{\"name\":\"Zoe\",\"plate\":\"AB-1\",\"location\":\"Hof 2\",\"active\":true}";
  cJSON *j = call("POST", "/api/admin/cars", b, tadm, 201, NULL);
  int64_t id = num(j, "id");
  TEST_ASSERT_EQUAL_STRING("Zoe", str(j, "name"));
  cJSON_Delete(j);
  char p[64];
  snprintf(p, sizeof p, "/api/admin/cars/%lld", (long long)id);
  j = call("PUT", p,
           "{\"name\":\"Zoe2\",\"plate\":\"AB-2\",\"location\":\"Hof 3\",\"active\":false}", tadm,
           200, NULL);
  TEST_ASSERT_EQUAL_STRING("Zoe2", str(j, "name"));
  cJSON_Delete(j);
  TEST_ASSERT_FALSE(db_car(&a->db, id)->active);
  j = call("GET", "/api/admin/cars", NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(j)); /* includes inactive */
  cJSON_Delete(j);
  adm_free("PUT", "/api/admin/cars/9999", b, 404, "not_found");
  adm_free("POST", "/api/admin/cars", "{\"name\":\"\",\"plate\":\"x\",\"location\":\"\",\"active\":true}",
           400, "invalid");
  adm_free("POST", "/api/admin/cars", "{\"name\":\"n\",\"plate\":\"x\",\"location\":\"\"}", 400,
           "invalid");
  adm_free("POST", "/api/admin/cars",
           "{\"name\":\"n\",\"plate\":\"0123456789abcdefg\",\"location\":\"\",\"active\":true}", 400,
           "invalid");
  TEST_ASSERT_EQUAL_INT(3, (int)a->db.n_cars);
}

void test_admin_create_booking_for_user(void) {
  cJSON *j = call("POST", "/api/admin/bookings", bk_body(car1, u1, T - 2 * H, T - H), tadm, 201, NULL);
  TEST_ASSERT_EQUAL_INT64(u1, num(j, "user_id"));
  TEST_ASSERT_EQUAL_INT64(adm, num(j, "modified_by"));
  cJSON_Delete(j); /* past start is fine for admin */
  adm_free("POST", "/api/admin/bookings", bk_body(car1, 9999, T + H, T + 2 * H), 400, "invalid");
  adm_free("POST", "/api/admin/bookings", bk_body(9999, u1, T + H, T + 2 * H), 400, "invalid");
  adm_free("POST", "/api/admin/bookings", "{\"car_id\":1}", 400, "invalid");
}

void test_admin_booking_overlap_409(void) {
  mk_booking(car1, u1, T + H, T + 3 * H);
  adm_free("POST", "/api/admin/bookings", bk_body(car1, adm, T + 2 * H, T + 4 * H), 409, "overlap");
  int64_t id = mk_booking(car1, u1, T + 3 * H, T + 4 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)id);
  adm_free("PUT", p, bk_body(car1, u1, T + 2 * H, T + 4 * H), 409, "overlap");
  TEST_ASSERT_EQUAL_INT64(T + 3 * H, db_booking(&a->db, id)->booked_start);
}

void test_admin_shorten_booking_ok(void) {
  int64_t id = mk_booking(car1, u1, T + H, T + 5 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)id);
  cJSON *j = call("PUT", p, bk_body(car2, adm, T + 2 * H, T + 3 * H), tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(T + 2 * H, num(j, "booked_start"));
  cJSON_Delete(j);
  booking_t *b = db_booking(&a->db, id);
  TEST_ASSERT_EQUAL_INT64(T + 2 * H, b->booked_start);
  TEST_ASSERT_EQUAL_INT64(T + 3 * H, b->booked_end);
  TEST_ASSERT_EQUAL_INT64(car2, b->car_id);
  TEST_ASSERT_EQUAL_INT64(adm, b->user_id);
  adm_free("PUT", "/api/admin/bookings/9999", bk_body(car1, u1, T, T + H), 404, "not_found");
}

void test_admin_cancel_booking_fee_zero(void) {
  int64_t id = mk_booking(car1, u1, T + 60, T + H); /* inside the fee deadline */
  char p[64];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)id);
  cJSON *j = call("DELETE", p, NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(0, num(j, "cancel_fee_cents"));
  TEST_ASSERT_EQUAL_STRING("cancelled", str(j, "state"));
  cJSON_Delete(j);
  booking_t *b = db_booking(&a->db, id);
  TEST_ASSERT_EQUAL_INT(BK_CANCELLED, b->state);
  TEST_ASSERT_EQUAL_INT64(T, b->cancelled_at);
  adm_free("DELETE", p, NULL, 400, "not_modifiable");
  adm_free("PUT", p, bk_body(car1, u1, T + H, T + 2 * H), 400, "not_modifiable");
  adm_free("DELETE", "/api/admin/bookings/9999", NULL, 404, "not_found");
}

void test_admin_bookings_list_filters(void) {
  int64_t b1 = mk_booking(car1, u1, T + H, T + 2 * H);
  int64_t b2 = mk_booking(car2, adm, T + 5 * H, T + 6 * H);
  int64_t b0 = mk_booking(car1, adm, T - 3 * H, T - 2 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)b2);
  adm_free("DELETE", p, NULL, 200, NULL);
  cJSON *j = call("GET", "/api/admin/bookings", NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(j)); /* includes cancelled */
  TEST_ASSERT_EQUAL_INT64(b0, num(cJSON_GetArrayItem(j, 0), "id"));
  TEST_ASSERT_EQUAL_INT64(b1, num(cJSON_GetArrayItem(j, 1), "id"));
  TEST_ASSERT_EQUAL_INT64(b2, num(cJSON_GetArrayItem(j, 2), "id"));
  TEST_ASSERT_EQUAL_STRING("anna@x.de", str(cJSON_GetArrayItem(j, 1), "user_email"));
  TEST_ASSERT_EQUAL_STRING("Test User", str(cJSON_GetArrayItem(j, 1), "user_name"));
  cJSON_Delete(j);

  char q[160];
  /* half-open: [T+2H, T+5H) touches b1 end and b2 start only */
  snprintf(q, sizeof q, "/api/admin/bookings?from=%lld&to=%lld", (long long)(T + 2 * H),
           (long long)(T + 5 * H));
  j = call("GET", q, NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(j));
  cJSON_Delete(j);
  snprintf(q, sizeof q, "/api/admin/bookings?from=%lld&to=%lld", (long long)(T + 2 * H - 60),
           (long long)(T + 5 * H + 60));
  j = call("GET", q, NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  cJSON_Delete(j);
  snprintf(q, sizeof q, "/api/admin/bookings?car=%lld", (long long)car1);
  j = call("GET", q, NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  cJSON_Delete(j);
  snprintf(q, sizeof q, "/api/admin/bookings?car=%lld&from=%lld&to=%lld", (long long)car1,
           (long long)T, (long long)(T + 10 * H));
  j = call("GET", q, NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT64(b1, num(cJSON_GetArrayItem(j, 0), "id"));
  cJSON_Delete(j);
  adm_free("GET", "/api/admin/bookings?from=abc&to=5", NULL, 400, "invalid");
  adm_free("GET", "/api/admin/bookings?car=x", NULL, 400, "invalid");
}

void test_settings_roundtrip_and_negative_400(void) {
  cJSON *j = call("GET", "/api/admin/settings", NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(15, num(j, "start_tolerance_min"));
  TEST_ASSERT_EQUAL_INT64(60, num(j, "cancel_deadline_min"));
  TEST_ASSERT_EQUAL_INT64(500, num(j, "cancel_fee_cents"));
  cJSON_Delete(j);
  j = call("PUT", "/api/admin/settings",
           "{\"start_tolerance_min\":5,\"cancel_deadline_min\":120,\"cancel_fee_cents\":1500}", tadm,
           200, NULL);
  TEST_ASSERT_EQUAL_INT64(120, num(j, "cancel_deadline_min"));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(5, a->db.settings.start_tolerance_min);
  TEST_ASSERT_EQUAL_INT64(1500, a->db.settings.cancel_fee_cents);
  j = call("GET", "/api/admin/settings", NULL, tadm, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(1500, num(j, "cancel_fee_cents"));
  cJSON_Delete(j);
  adm_free("PUT", "/api/admin/settings",
           "{\"start_tolerance_min\":-1,\"cancel_deadline_min\":1,\"cancel_fee_cents\":1}", 400, "invalid");
  adm_free("PUT", "/api/admin/settings",
           "{\"start_tolerance_min\":1,\"cancel_deadline_min\":-1,\"cancel_fee_cents\":1}", 400, "invalid");
  adm_free("PUT", "/api/admin/settings",
           "{\"start_tolerance_min\":1,\"cancel_deadline_min\":1,\"cancel_fee_cents\":-1}", 400, "invalid");
  adm_free("PUT", "/api/admin/settings",
           "{\"start_tolerance_min\":2147483648,\"cancel_deadline_min\":1,\"cancel_fee_cents\":1}", 400,
           "invalid");
  adm_free("PUT", "/api/admin/settings",
           "{\"start_tolerance_min\":1,\"cancel_deadline_min\":1,\"cancel_fee_cents\":9007199254740994}",
           400, "invalid");
  adm_free("PUT", "/api/admin/settings",
           "{\"start_tolerance_min\":1,\"cancel_deadline_min\":1,\"cancel_fee_cents\":1000000000000000}",
           400, "invalid");
  adm_free("PUT", "/api/admin/settings", "{\"start_tolerance_min\":1,\"cancel_deadline_min\":1}", 400,
           "invalid");
  adm_free("PUT", "/api/admin/settings",
           "{\"start_tolerance_min\":2147483647,\"cancel_deadline_min\":0,"
           "\"cancel_fee_cents\":999999999999999}", 200, NULL);
  TEST_ASSERT_EQUAL_INT64(999999999999999LL, a->db.settings.cancel_fee_cents);
  db_t d; /* the largest fee survives a reload unchanged */
  TEST_ASSERT_EQUAL_INT(0, db_load(&d, a->cfg.data_dir));
  TEST_ASSERT_EQUAL_INT64(999999999999999LL, d.settings.cancel_fee_cents);
  db_free(&d);
  TEST_ASSERT_EQUAL_INT(INT_MAX, a->db.settings.start_tolerance_min);
  TEST_ASSERT_EQUAL_INT(0, a->db.settings.cancel_deadline_min);
}

void test_storage_failure_500(void) {
  if (geteuid() == 0) TEST_IGNORE_MESSAGE("root ignores directory permissions");
  TEST_ASSERT_EQUAL_INT(0, chmod(a->cfg.data_dir, 0500));
  api_res_t r1 = th_req(a, "POST", "/api/admin/cars",
                        "{\"name\":\"n\",\"plate\":\"p\",\"location\":\"l\",\"active\":true}", tadm, T);
  api_res_t r2 = th_req(a, "PUT", "/api/admin/settings",
                        "{\"start_tolerance_min\":1,\"cancel_deadline_min\":1,\"cancel_fee_cents\":1}",
                        tadm, T);
  api_res_t r3 = th_req(a, "POST", "/api/admin/users", user_body("z@x.de", "user"), tadm, T);
  int s1 = r1.status, s2 = r2.status, s3 = r3.status;
  int c1 = r1.body && strstr(r1.body, "\"storage\"");
  int c3 = r3.body && strstr(r3.body, "\"storage\"");
  api_res_free(&r1);
  api_res_free(&r2);
  api_res_free(&r3);
  chmod(a->cfg.data_dir, 0700);
  TEST_ASSERT_EQUAL_INT(500, s1);
  TEST_ASSERT_EQUAL_INT(500, s2);
  TEST_ASSERT_EQUAL_INT(500, s3);
  TEST_ASSERT_TRUE(c1);
  TEST_ASSERT_TRUE(c3);
  TEST_ASSERT_EQUAL_INT(2, (int)a->db.n_cars);
  TEST_ASSERT_NULL(db_user_by_email(&a->db, "z@x.de"));
  TEST_ASSERT_EQUAL_INT(15, a->db.settings.start_tolerance_min);
}

/* C1: admin times and license_checked_at must lie in [0, now + 2 years]. */
#define YEAR2 (2 * 365 * 86400)

static void assert_dir_loads(void) {
  db_t d;
  TEST_ASSERT_EQUAL_INT(0, db_load(&d, a->cfg.data_dir));
  db_free(&d);
}

void test_admin_time_out_of_range_400(void) {
  adm_free("POST", "/api/admin/bookings",
           "{\"car_id\":1,\"user_id\":2,\"start\":-9223372036854775808,\"end\":0}", 400,
           "invalid");
  adm_free("POST", "/api/admin/bookings", bk_body(car1, u1, -H, H), 400, "invalid");
  adm_free("POST", "/api/admin/bookings", bk_body(car1, u1, T, T + YEAR2 + 60), 400, "invalid");
  adm_free("POST", "/api/admin/bookings",
           "{\"car_id\":1,\"user_id\":2,\"start\":1774746000,\"end\":4323455642275676160}",
           400, "invalid");
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_bookings);
  int64_t id = mk_booking(car1, u1, T + H, T + 2 * H);
  char p[64];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)id);
  adm_free("PUT", p, bk_body(car1, u1, -9223372036854775807LL - 1 + 32, T + 2 * H), 400,
           "invalid");
  adm_free("PUT", p, bk_body(car1, u1, T + H, T + YEAR2 + 60), 400, "invalid");
  TEST_ASSERT_EQUAL_INT64(T + H, db_booking(&a->db, id)->booked_start);
  TEST_ASSERT_EQUAL_INT64(T + 2 * H, db_booking(&a->db, id)->booked_end);
  assert_dir_loads();
}

void test_admin_license_checked_at_out_of_range_400(void) {
  char p[64], b[400];
  snprintf(p, sizeof p, "/api/admin/users/%lld", (long long)u1);
  const char *bad[] = {"4323455642275676160", "-1", "1837814460" /* T + 2y + 60 */};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    snprintf(b, sizeof b,
             "{\"email\":\"anna@x.de\",\"name\":\"Anna\",\"street\":\"S\",\"zip\":\"1\","
             "\"city\":\"C\",\"role\":\"user\",\"active\":true,\"license_checked_at\":%s}",
             bad[i]);
    adm_free("PUT", p, b, 400, "invalid");
  }
  TEST_ASSERT_EQUAL_INT64(0, db_user(&a->db, u1)->license_checked_at);
  snprintf(b, sizeof b,
           "{\"email\":\"anna@x.de\",\"name\":\"Anna\",\"street\":\"S\",\"zip\":\"1\","
           "\"city\":\"C\",\"role\":\"user\",\"active\":true,\"license_checked_at\":%lld}",
           (long long)T);
  adm_free("PUT", p, b, 200, NULL);
  TEST_ASSERT_EQUAL_INT64(T, db_user(&a->db, u1)->license_checked_at);
  assert_dir_loads();
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_user_role_gets_403_on_admin);
  RUN_TEST(test_admin_create_user_sends_invite);
  RUN_TEST(test_admin_create_user_validation);
  RUN_TEST(test_admin_create_duplicate_email_409);
  RUN_TEST(test_admin_create_user_mail_failure);
  RUN_TEST(test_invited_user_cannot_login_before_set);
  RUN_TEST(test_admin_users_list_public);
  RUN_TEST(test_admin_user_put_updates);
  RUN_TEST(test_admin_user_put_duplicate_email_409);
  RUN_TEST(test_deactivate_user_kills_session);
  RUN_TEST(test_admin_cannot_deactivate_self);
  RUN_TEST(test_admin_invite_resend);
  RUN_TEST(test_admin_cars_crud);
  RUN_TEST(test_admin_create_booking_for_user);
  RUN_TEST(test_admin_booking_overlap_409);
  RUN_TEST(test_admin_shorten_booking_ok);
  RUN_TEST(test_admin_cancel_booking_fee_zero);
  RUN_TEST(test_admin_bookings_list_filters);
  RUN_TEST(test_settings_roundtrip_and_negative_400);
  RUN_TEST(test_storage_failure_500);
  RUN_TEST(test_admin_time_out_of_range_400);
  RUN_TEST(test_admin_license_checked_at_out_of_range_400);
  return UNITY_END();
}
