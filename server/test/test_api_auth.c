#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/session.h"
#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400
#define COOKIE_ATTRS "HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=2592000"

static app_t *a;
static int64_t uid;

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  uid = th_add_user(a, "anna@x.de", "correct horse", ROLE_USER);
  TEST_ASSERT_TRUE(uid > 0);
}

void tearDown(void) { th_free(a); }

static void expect(api_res_t *r, int status, const char *code) {
  TEST_ASSERT_NOT_NULL(r->body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(status, r->status, r->body);
  if (code) {
    cJSON *j = cJSON_Parse(r->body);
    TEST_ASSERT_NOT_NULL(j);
    cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
    TEST_ASSERT_TRUE(cJSON_IsString(e));
    TEST_ASSERT_EQUAL_STRING(code, e->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "message")));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
    cJSON_Delete(j);
  }
}

static void req_expect(const char *m, const char *p, const char *body, const char *tok,
                       int status, const char *code) {
  api_res_t r = th_req(a, m, p, body, tok, T);
  expect(&r, status, code);
  api_res_free(&r);
}

static const char *LOGIN = "{\"email\":\"anna@x.de\",\"password\":\"correct horse\"}";
static const char *LOGIN_BAD = "{\"email\":\"anna@x.de\",\"password\":\"wrong horse!\"}";

void test_login_ok_sets_cookie(void) {
  api_res_t r = th_req(a, "POST", "/api/login", LOGIN, NULL, T);
  expect(&r, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, strncmp(r.set_cookie, "ocs_session=", 12));
  TEST_ASSERT_NOT_NULL(strstr(r.set_cookie, COOKIE_ATTRS));
  cJSON *j = cJSON_Parse(r.body);
  cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "user");
  TEST_ASSERT_TRUE(cJSON_IsObject(u));
  TEST_ASSERT_EQUAL_STRING("anna@x.de", cJSON_GetObjectItemCaseSensitive(u, "email")->valuestring);
  TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(u, "pw_hash"));
  cJSON_Delete(j);
  char tok[65];
  memcpy(tok, r.set_cookie + 12, 64);
  tok[64] = 0;
  api_res_free(&r);
  req_expect("GET", "/api/me", NULL, tok, 200, NULL);
}

void test_login_email_case_insensitive(void) {
  req_expect("POST", "/api/login", "{\"email\":\"ANNA@X.de\",\"password\":\"correct horse\"}",
             NULL, 200, NULL);
}

void test_login_wrong_pw_401(void) {
  req_expect("POST", "/api/login", LOGIN_BAD, NULL, 401, "bad_credentials");
  req_expect("POST", "/api/login", "{\"email\":\"nobody@x.de\",\"password\":\"correct horse\"}",
             NULL, 401, "bad_credentials");
}

void test_login_inactive_or_pwless_401(void) {
  user_t u = *db_user(&a->db, uid);
  u.active = false;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
  req_expect("POST", "/api/login", LOGIN, NULL, 401, "bad_credentials");
  TEST_ASSERT_TRUE(th_add_user(a, "nopw@x.de", NULL, ROLE_USER) > 0);
  req_expect("POST", "/api/login", "{\"email\":\"nopw@x.de\",\"password\":\"\"}", NULL, 401,
             "bad_credentials");
}

void test_login_rate_limited_after_5_fails(void) {
  for (int i = 0; i < 5; i++) req_expect("POST", "/api/login", LOGIN_BAD, NULL, 401, "bad_credentials");
  req_expect("POST", "/api/login", LOGIN, NULL, 429, "rate_limited");
}

void test_login_success_resets_rate_limit(void) {
  for (int i = 0; i < 4; i++) req_expect("POST", "/api/login", LOGIN_BAD, NULL, 401, "bad_credentials");
  req_expect("POST", "/api/login", LOGIN, NULL, 200, NULL);
  for (int i = 0; i < 4; i++) req_expect("POST", "/api/login", LOGIN_BAD, NULL, 401, "bad_credentials");
  req_expect("POST", "/api/login", LOGIN, NULL, 200, NULL);
}

void test_login_long_password_400(void) {
  char *body = malloc(1200);
  char pw[1026];
  memset(pw, 'p', 1025);
  pw[1025] = 0;
  snprintf(body, 1200, "{\"email\":\"anna@x.de\",\"password\":\"%s\"}", pw);
  req_expect("POST", "/api/login", body, NULL, 400, "invalid");
  free(body);
}

void test_unknown_path_404(void) {
  req_expect("GET", "/api/nope", NULL, NULL, 404, "not_found");
  req_expect("GET", "/api/me/x", NULL, NULL, 404, "not_found");
}

void test_wrong_method_405(void) {
  req_expect("GET", "/api/login", NULL, NULL, 405, "method_not_allowed");
  req_expect("DELETE", "/api/me", NULL, NULL, 405, "method_not_allowed");
}

void test_me_requires_session_401(void) {
  req_expect("GET", "/api/me", NULL, NULL, 401, "unauthenticated");
  req_expect("GET", "/api/me", NULL, "garbage", 401, "unauthenticated");
  req_expect("PUT", "/api/me", "{}", NULL, 401, "unauthenticated");
}

void test_non_json_content_type_415(void) {
  api_res_t r = th_req_ct(a, "POST", "/api/login", LOGIN, NULL, T, "text/plain");
  expect(&r, 415, "unsupported_media_type");
  api_res_free(&r);
  r = th_req_ct(a, "POST", "/api/logout", NULL, NULL, T, NULL);
  expect(&r, 415, "unsupported_media_type");
  api_res_free(&r);
  r = th_req_ct(a, "POST", "/api/login", LOGIN, NULL, T, "application/jsonx");
  expect(&r, 415, "unsupported_media_type");
  api_res_free(&r);
  r = th_req_ct(a, "POST", "/api/login", LOGIN, NULL, T, "Application/JSON; charset=utf-8");
  expect(&r, 200, NULL);
  api_res_free(&r);
  r = th_req_ct(a, "GET", "/api/me", NULL, NULL, T, NULL); /* GET needs no content type */
  expect(&r, 401, "unauthenticated");
  api_res_free(&r);
}

void test_malformed_json_400(void) {
  req_expect("POST", "/api/login", "{\"email\":", NULL, 400, "invalid_json");
  req_expect("POST", "/api/login", "[1]", NULL, 400, "invalid_json");
  req_expect("POST", "/api/login", "{} x", NULL, 400, "invalid_json");
}

void test_embedded_nul_body_400(void) {
  api_req_t q;
  memset(&q, 0, sizeof q);
  q.method = "POST";
  q.path = "/api/login";
  q.body = "{}\0{";
  q.body_len = 4;
  q.content_type = "application/json";
  q.client_ip = "1.2.3.4";
  q.now = T;
  api_res_t r;
  api_handle(a, &q, &r);
  expect(&r, 400, "invalid_json");
  api_res_free(&r);
}

void test_wrong_field_type_400(void) {
  req_expect("POST", "/api/login", "{\"email\":5,\"password\":\"x\"}", NULL, 400, "invalid");
  req_expect("POST", "/api/login", "{\"password\":\"x\"}", NULL, 400, "invalid");
  req_expect("POST", "/api/login", NULL, NULL, 400, "invalid");
}

void test_forgot_unknown_email_200_no_mail(void) {
  req_expect("POST", "/api/password/forgot", "{\"email\":\"nobody@x.de\"}", NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
}

void test_forgot_inactive_user_no_mail(void) {
  user_t u = *db_user(&a->db, uid);
  u.active = false;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
  req_expect("POST", "/api/password/forgot", "{\"email\":\"anna@x.de\"}", NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
}

static const char *mail_token(void) {
  static char tok[65];
  const char *p = strstr(th_last_mail_body(), TH_BASE_URL "/#/passwort?token=");
  TEST_ASSERT_NOT_NULL(p);
  p += strlen(TH_BASE_URL "/#/passwort?token=");
  memcpy(tok, p, 64);
  tok[64] = 0;
  return tok;
}

void test_forgot_then_set_password_then_login(void) {
  char *old = th_login_at(a, "anna@x.de", "correct horse", T);
  TEST_ASSERT_NOT_NULL(old);
  char oldtok[65];
  strcpy(oldtok, old);
  req_expect("POST", "/api/password/forgot", "{\"email\":\"Anna@x.de\"}", NULL, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("anna@x.de", th_last_mail_to());
  TEST_ASSERT_EQUAL_STRING("Passwort zurücksetzen", th_last_mail_subject());
  const char *tok = mail_token();
  char body[256];
  snprintf(body, sizeof body, "{\"token\":\"%s\",\"password\":\"short\"}", tok);
  req_expect("POST", "/api/password/set", body, NULL, 400, "password_too_short");
  snprintf(body, sizeof body, "{\"token\":\"%s\",\"password\":\"brand new pw\"}", tok);
  req_expect("POST", "/api/password/set", body, NULL, 200, NULL);
  req_expect("POST", "/api/password/set", body, NULL, 400, "invalid_token");
  req_expect("GET", "/api/me", NULL, oldtok, 401, "unauthenticated");
  req_expect("POST", "/api/login", LOGIN, NULL, 401, "bad_credentials");
  req_expect("POST", "/api/login", "{\"email\":\"anna@x.de\",\"password\":\"brand new pw\"}", NULL,
             200, NULL);
}

void test_set_password_with_invite_token(void) {
  int64_t id = th_add_user(a, "neu@x.de", NULL, ROLE_USER);
  char tok[65], body[256];
  TEST_ASSERT_EQUAL_INT(0, token_issue(a, id, TOKEN_INVITE, T, tok));
  snprintf(body, sizeof body, "{\"token\":\"%s\",\"password\":\"äöüäöüäöüä\"}", tok);
  req_expect("POST", "/api/password/set", body, NULL, 200, NULL);
  TEST_ASSERT_NOT_NULL(th_login_at(a, "neu@x.de", "äöüäöüäöüä", T));
}

void test_logout(void) {
  char *t = th_login_at(a, "anna@x.de", "correct horse", T);
  TEST_ASSERT_NOT_NULL(t);
  char tok[65];
  strcpy(tok, t);
  api_res_t r = th_req(a, "POST", "/api/logout", NULL, tok, T);
  expect(&r, 200, NULL);
  TEST_ASSERT_EQUAL_STRING(
      "ocs_session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0", r.set_cookie);
  api_res_free(&r);
  req_expect("GET", "/api/me", NULL, tok, 401, "unauthenticated");
  req_expect("POST", "/api/logout", NULL, NULL, 200, NULL);
}

void test_me_put_updates_profile(void) {
  char tok[65];
  strcpy(tok, th_login_at(a, "anna@x.de", "correct horse", T));
  req_expect("PUT", "/api/me", "{\"name\":\"Anna\",\"street\":\"Weg 1\",\"zip\":\"12345\"}", tok,
             400, "invalid");
  char longname[200];
  memset(longname, 'n', 120);
  longname[120] = 0;
  char body[400];
  snprintf(body, sizeof body,
           "{\"name\":\"%s\",\"street\":\"Weg 1\",\"zip\":\"12345\",\"city\":\"Ort\"}", longname);
  req_expect("PUT", "/api/me", body, tok, 400, "invalid");
  api_res_t r = th_req(a, "PUT", "/api/me",
                       "{\"name\":\"Anna\",\"street\":\"Weg 1\",\"zip\":\"12345\",\"city\":\"Ort\","
                       "\"role\":\"admin\",\"email\":\"evil@x.de\"}",
                       tok, T);
  expect(&r, 200, NULL);
  api_res_free(&r);
  user_t *u = db_user(&a->db, uid);
  TEST_ASSERT_EQUAL_STRING("Anna", u->name);
  TEST_ASSERT_EQUAL_STRING("Ort", u->city);
  TEST_ASSERT_EQUAL_STRING("anna@x.de", u->email);
  TEST_ASSERT_EQUAL_INT(ROLE_USER, u->role);
}

void test_change_password_kills_other_sessions(void) {
  char t1[65], t2[65];
  strcpy(t1, th_login_at(a, "anna@x.de", "correct horse", T));
  strcpy(t2, th_login_at(a, "anna@x.de", "correct horse", T));
  req_expect("PUT", "/api/me/password", "{\"current\":\"wrong horse!\",\"new\":\"brand new pw\"}", t1,
             400, "bad_credentials");
  req_expect("PUT", "/api/me/password", "{\"current\":\"correct horse\",\"new\":\"short\"}", t1, 400,
             "password_too_short");
  req_expect("PUT", "/api/me/password", "{\"current\":\"correct horse\",\"new\":\"brand new pw\"}",
             t1, 200, NULL);
  req_expect("GET", "/api/me", NULL, t1, 200, NULL);
  req_expect("GET", "/api/me", NULL, t2, 401, "unauthenticated");
  TEST_ASSERT_NOT_NULL(th_login_at(a, "anna@x.de", "brand new pw", T));
}

void test_admin_role_enum_values(void) {
  /* sanity: user role JSON is reported via /api/me */
  int64_t id = th_add_user(a, "admin@x.de", "admin password", ROLE_ADMIN);
  TEST_ASSERT_TRUE(id > 0);
  char *t = th_login_at(a, "admin@x.de", "admin password", T);
  TEST_ASSERT_NOT_NULL(t);
  api_res_t r = th_req(a, "GET", "/api/me", NULL, t, T);
  expect(&r, 200, NULL);
  TEST_ASSERT_NOT_NULL(strstr(r.body, "\"role\":\"admin\""));
  api_res_free(&r);
}

void test_path_id_parsing(void) {
  int64_t id = 0;
  TEST_ASSERT_TRUE(api_path_id("/api/bookings/12", "/api/bookings/", &id));
  TEST_ASSERT_EQUAL_INT64(12, id);
  TEST_ASSERT_TRUE(api_path_id("/api/bookings/7", "/api/bookings", &id));
  TEST_ASSERT_EQUAL_INT64(7, id);
  TEST_ASSERT_TRUE(api_path_id("/api/bookings/9223372036854775807", "/api/bookings/", &id));
  TEST_ASSERT_FALSE(api_path_id("/api/bookings/9223372036854775808", "/api/bookings/", &id));
  TEST_ASSERT_FALSE(api_path_id("/api/bookings/abc", "/api/bookings/", &id));
  TEST_ASSERT_FALSE(api_path_id("/api/bookings/", "/api/bookings/", &id));
  TEST_ASSERT_FALSE(api_path_id("/api/bookings/+1", "/api/bookings/", &id));
  TEST_ASSERT_FALSE(api_path_id("/api/bookings/-1", "/api/bookings/", &id));
  TEST_ASSERT_FALSE(api_path_id("/api/bookings/1x", "/api/bookings/", &id));
  TEST_ASSERT_FALSE(api_path_id("/api/bookings/1/", "/api/bookings/", &id));
  TEST_ASSERT_FALSE(api_path_id("/api/bookings12", "/api/bookings", &id));
  TEST_ASSERT_FALSE(api_path_id("/api/cars/1", "/api/bookings/", &id));
}

void test_query_int_parsing(void) {
  int64_t v = 0;
  TEST_ASSERT_TRUE(api_query_int("car=1&from=100&to=-5", "from", &v));
  TEST_ASSERT_EQUAL_INT64(100, v);
  TEST_ASSERT_TRUE(api_query_int("car=1&from=100&to=-5", "to", &v));
  TEST_ASSERT_EQUAL_INT64(-5, v);
  TEST_ASSERT_TRUE(api_query_int("car=1", "car", &v));
  TEST_ASSERT_FALSE(api_query_int("xcar=1", "car", &v));
  TEST_ASSERT_FALSE(api_query_int("car=", "car", &v));
  TEST_ASSERT_FALSE(api_query_int("car=1x", "car", &v));
  TEST_ASSERT_FALSE(api_query_int("car=99999999999999999999", "car", &v));
  TEST_ASSERT_FALSE(api_query_int(NULL, "car", &v));
  TEST_ASSERT_FALSE(api_query_int("car", "car", &v));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_login_ok_sets_cookie);
  RUN_TEST(test_login_email_case_insensitive);
  RUN_TEST(test_login_wrong_pw_401);
  RUN_TEST(test_login_inactive_or_pwless_401);
  RUN_TEST(test_login_rate_limited_after_5_fails);
  RUN_TEST(test_login_success_resets_rate_limit);
  RUN_TEST(test_login_long_password_400);
  RUN_TEST(test_unknown_path_404);
  RUN_TEST(test_wrong_method_405);
  RUN_TEST(test_me_requires_session_401);
  RUN_TEST(test_non_json_content_type_415);
  RUN_TEST(test_malformed_json_400);
  RUN_TEST(test_embedded_nul_body_400);
  RUN_TEST(test_wrong_field_type_400);
  RUN_TEST(test_forgot_unknown_email_200_no_mail);
  RUN_TEST(test_forgot_inactive_user_no_mail);
  RUN_TEST(test_forgot_then_set_password_then_login);
  RUN_TEST(test_set_password_with_invite_token);
  RUN_TEST(test_logout);
  RUN_TEST(test_me_put_updates_profile);
  RUN_TEST(test_change_password_kills_other_sessions);
  RUN_TEST(test_admin_role_enum_values);
  RUN_TEST(test_path_id_parsing);
  RUN_TEST(test_query_int_parsing);
  return UNITY_END();
}
