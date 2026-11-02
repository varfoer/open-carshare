#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400

static app_t *a;
static int64_t uid;
static const char *tok;
static char tokbuf[65];

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  uid = th_add_user_bare(a, "anna@x.de", "correct horse", ROLE_USER);
  TEST_ASSERT_TRUE(uid > 0);
  const char *t = th_login_at(a, "anna@x.de", "correct horse", T);
  TEST_ASSERT_NOT_NULL(t);
  memcpy(tokbuf, t, sizeof tokbuf);
  tok = tokbuf;
}

void tearDown(void) { th_free(a); }

static cJSON *do_req(const char *m, const char *body, int status, const char *code) {
  api_res_t r = th_req(a, m, "/api/me/mandate", body, tok, T);
  TEST_ASSERT_NOT_NULL(r.body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(status, r.status, r.body);
  TEST_ASSERT_NULL(strstr(r.body, "0532013000"));
  TEST_ASSERT_NULL(strstr(r.body, "98765432"));
  cJSON *j = cJSON_Parse(r.body);
  api_res_free(&r);
  TEST_ASSERT_NOT_NULL(j);
  if (code) TEST_ASSERT_EQUAL_STRING(code, cJSON_GetObjectItemCaseSensitive(j, "error")->valuestring);
  return j;
}

static void post(const char *body, int status, const char *code) {
  cJSON_Delete(do_req("POST", body, status, code));
}

static const char *GOOD =
    "{\"account_holder\":\"Anna Beispiel\",\"iban\":\"de89 3704 0044 0532 0130 00\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":1,\"accept\":true}";

static const char *str(const cJSON *o, const char *k) {
  const cJSON *e = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsString(e));
  return e->valuestring;
}

void test_get_not_ready(void) {
  cJSON *j = do_req("GET", NULL, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "ready")));
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "text")));
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "creditor_id")));
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "mandate")));
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItemCaseSensitive(j, "text_version")->valueint);
  cJSON_Delete(j);
}

void test_get_ready_has_text(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  cJSON *j = do_req("GET", NULL, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ready")));
  TEST_ASSERT_NOT_NULL(strstr(str(j, "text"), "Carsharing Testverein e.V."));
  TEST_ASSERT_EQUAL_STRING("DE98ZZZ09999999999", str(j, "creditor_id"));
  cJSON_Delete(j);
}

void test_post_not_ready_409(void) { post(GOOD, 409, "no_creditor"); }

void test_post_mandate_normalizes(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  cJSON *j = do_req("POST", GOOD, 201, NULL);
  cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "mandate");
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "changed")));
  TEST_ASSERT_EQUAL_STRING("OCS-000001", str(m, "ref"));
  TEST_ASSERT_EQUAL_STRING("DE89 **** 3000", str(m, "iban_masked"));
  TEST_ASSERT_EQUAL_STRING("1.2.3.4", str(m, "client_ip"));
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "mail_sent")));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, (int)a->db.n_mandates);
  TEST_ASSERT_EQUAL_STRING("DE89370400440532013000", a->db.mandates[0].iban);
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("anna@x.de", th_last_mail_to());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "OCS-000001"));
  TEST_ASSERT_NULL(strstr(th_last_mail_body(), "0532013000"));
}

void test_post_rejects_bad_input(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013001\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":1,\"accept\":true}", 400, "invalid_iban");
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"bic\":\"XX\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":1,\"accept\":true}", 400, "invalid_bic");
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":1,\"accept\":false}", 400, "invalid");
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":1}", 400, "invalid");
  post("{\"account_holder\":\"  \",\"iban\":\"DE89370400440532013000\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":1,\"accept\":true}", 400, "invalid");
  char body[400];
  snprintf(body, sizeof body,
           "{\"account_holder\":\"%071d\",\"iban\":\"DE89370400440532013000\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":1,\"accept\":true}", 0);
  post(body, 400, "invalid");
  TEST_ASSERT_EQUAL_INT(0, (int)a->db.n_mandates);
}

void test_post_stale_text_version_409(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":0,\"accept\":true}", 409, "text_changed");
}

void test_post_stale_creditor_409(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"creditor_id\":\"DE98ZZZ00000000001\",\"text_version\":1,\"accept\":true}", 409, "text_changed");
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"text_version\":1,\"accept\":true}", 400, "invalid");
  /* spaces/lowercase are normalized before the compare */
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"creditor_id\":\"de98 zzz0 9999 9999 99\",\"text_version\":1,\"accept\":true}", 201, NULL);
}

void test_post_same_data_is_idempotent(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  post(GOOD, 201, NULL);
  cJSON *j = do_req("POST", GOOD, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "mail_sent")));
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "changed")));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, (int)a->db.n_mandates);
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
}

void test_post_new_iban_replaces(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  post(GOOD, 201, NULL);
  post("{\"account_holder\":\"Anna Beispiel\",\"iban\":\"GB82WEST12345698765432\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":1,\"accept\":true}", 201, NULL);
  TEST_ASSERT_EQUAL_INT(2, (int)a->db.n_mandates);
  TEST_ASSERT_EQUAL_INT(MANDATE_REPLACED, a->db.mandates[0].status);
  TEST_ASSERT_EQUAL_INT64(T, a->db.mandates[0].ended_at);
  TEST_ASSERT_EQUAL_INT64(uid, a->db.mandates[0].ended_by);
  const mandate_t *c = db_mandate_current(&a->db, uid);
  TEST_ASSERT_NOT_NULL(c);
  TEST_ASSERT_EQUAL_INT64(2, c->id);
  cJSON *j = do_req("GET", NULL, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("OCS-000002", str(cJSON_GetObjectItemCaseSensitive(j, "mandate"), "ref"));
  cJSON_Delete(j);
}

void test_mandate_keeps_creditor_snapshot(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  post(GOOD, 201, NULL);
  settings_t s = a->db.settings;
  snprintf(s.assoc_name, sizeof s.assoc_name, "Neuer Name e.V.");
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&a->db, &s));
  cJSON *j = do_req("GET", NULL, 200, NULL);
  TEST_ASSERT_EQUAL_STRING("Carsharing Testverein e.V.",
                           str(cJSON_GetObjectItemCaseSensitive(j, "mandate"), "creditor_name"));
  cJSON_Delete(j);
}

void test_post_mail_failure_still_201(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  th_mail_fail(1);
  cJSON *j = do_req("POST", GOOD, 201, NULL);
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "mail_sent")));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, (int)a->db.n_mandates);
}

void test_post_heals_second_active_row(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  mandate_t m;
  memset(&m, 0, sizeof m);
  m.user_id = uid;
  m.status = MANDATE_ACTIVE;
  m.created_at = 1;
  m.text_version = 1;
  snprintf(m.holder, sizeof m.holder, "Anna Beispiel");
  snprintf(m.iban, sizeof m.iban, "GB82WEST12345698765432");
  TEST_ASSERT_EQUAL_INT(0, db_put_mandate(&a->db, &m));
  memset(&m, 0, sizeof m);
  m.user_id = uid;
  m.status = MANDATE_ACTIVE;
  m.created_at = 2;
  m.text_version = 1;
  snprintf(m.holder, sizeof m.holder, "Anna Beispiel");
  snprintf(m.iban, sizeof m.iban, "DE89370400440532013000");
  TEST_ASSERT_EQUAL_INT(0, db_put_mandate(&a->db, &m));
  cJSON_Delete(do_req("POST", GOOD, 200, NULL));
  int active = 0;
  for (size_t i = 0; i < a->db.n_mandates; i++) active += a->db.mandates[i].status == MANDATE_ACTIVE;
  TEST_ASSERT_EQUAL_INT(1, active);
  TEST_ASSERT_EQUAL_INT(MANDATE_REPLACED, a->db.mandates[0].status);
  TEST_ASSERT_EQUAL_INT64(T, a->db.mandates[0].ended_at);
  TEST_ASSERT_EQUAL_INT64(2, db_mandate_current(&a->db, uid)->id);
}

/* ---- revoke ---- */

/* th_login_at returns a shared static buffer: copy before the next login. */
static void login_copy(char out[65], const char *email, const char *pw) {
  const char *t = th_login_at(a, email, pw, T);
  TEST_ASSERT_NOT_NULL(t);
  memcpy(out, t, 65);
}

static int64_t add_booking(int64_t user, booking_state_t st, int64_t start) {
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = 1;
  b.user_id = user;
  b.booked_start = start;
  b.booked_end = start + 3600;
  b.state = st;
  b.created_at = T - 100;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  return b.id;
}

static int64_t add_mandate(int64_t user) {
  mandate_t m;
  memset(&m, 0, sizeof m);
  m.user_id = user;
  m.status = MANDATE_ACTIVE;
  m.created_at = T - 1000;
  snprintf(m.holder, sizeof m.holder, "Anna Beispiel");
  snprintf(m.iban, sizeof m.iban, "DE89370400440532013000");
  TEST_ASSERT_EQUAL_INT(0, db_put_mandate(&a->db, &m));
  return m.id;
}

static int n_sessions_of(int64_t user) {
  int n = 0;
  for (size_t i = 0; i < a->db.n_sessions; i++) n += a->db.sessions[i].user_id == user;
  return n;
}

static void check_revoked(int64_t user, int64_t actor, int64_t mid, int64_t res_id, int64_t act_id,
                          int64_t other_id) {
  TEST_ASSERT_EQUAL_INT(MANDATE_REVOKED, db_mandate(&a->db, mid)->status);
  TEST_ASSERT_EQUAL_INT64(actor, db_mandate(&a->db, mid)->ended_by);
  TEST_ASSERT_EQUAL_INT64(T, db_mandate(&a->db, mid)->ended_at);
  TEST_ASSERT_EQUAL_INT(BK_CANCELLED, db_booking(&a->db, res_id)->state);
  TEST_ASSERT_EQUAL_INT64(0, db_booking(&a->db, res_id)->cancel_fee_cents);
  TEST_ASSERT_EQUAL_INT64(T, db_booking(&a->db, res_id)->cancelled_at);
  TEST_ASSERT_EQUAL_INT64(actor, db_booking(&a->db, res_id)->modified_by);
  TEST_ASSERT_EQUAL_INT(BK_CANCELLED, db_booking(&a->db, other_id)->state);
  TEST_ASSERT_EQUAL_INT64(0, db_booking(&a->db, other_id)->cancel_fee_cents);
  if (act_id) TEST_ASSERT_EQUAL_INT(BK_ACTIVE, db_booking(&a->db, act_id)->state);
  TEST_ASSERT_FALSE(db_user(&a->db, user)->active);
  TEST_ASSERT_EQUAL_INT(0, n_sessions_of(user));
}

void test_revoke_cancels_reserved(void) {
  settings_t s = a->db.settings;
  s.cancel_fee_cents = 500;
  s.cancel_deadline_min = 1440;
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&a->db, &s));
  int64_t mid = add_mandate(uid);
  int64_t r1 = add_booking(uid, BK_RESERVED, T + 600);
  int64_t r2 = add_booking(uid, BK_RESERVED, T + 86400 * 5);
  api_res_t r = th_req(a, "DELETE", "/api/me/mandate", "{\"password\":\"correct horse\"}", tok, T);
  TEST_ASSERT_EQUAL_INT_MESSAGE(200, r.status, r.body);
  TEST_ASSERT_NOT_NULL(strstr(r.set_cookie, "ocs_session=;"));
  TEST_ASSERT_NOT_NULL(strstr(r.set_cookie, "Max-Age=0"));
  api_res_free(&r);
  check_revoked(uid, uid, mid, r1, 0, r2);
}

void test_revoke_during_trip_409(void) {
  int64_t mid = add_mandate(uid);
  int64_t act = add_booking(uid, BK_ACTIVE, T - 600);
  int64_t r1 = add_booking(uid, BK_RESERVED, T + 600);
  api_res_t r = th_req(a, "DELETE", "/api/me/mandate", "{\"password\":\"correct horse\"}", tok, T);
  TEST_ASSERT_EQUAL_INT_MESSAGE(409, r.status, r.body);
  TEST_ASSERT_NOT_NULL(strstr(r.body, "trip_active"));
  api_res_free(&r);
  TEST_ASSERT_EQUAL_INT(MANDATE_ACTIVE, db_mandate(&a->db, mid)->status);
  TEST_ASSERT_EQUAL_INT(BK_ACTIVE, db_booking(&a->db, act)->state);
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, db_booking(&a->db, r1)->state);
  TEST_ASSERT_TRUE(db_user(&a->db, uid)->active);
  TEST_ASSERT_EQUAL_INT(1, n_sessions_of(uid));
}

void test_revoke_wrong_password_400(void) {
  int64_t mid = add_mandate(uid);
  int64_t r1 = add_booking(uid, BK_RESERVED, T + 600);
  const char *bodies[] = {"{\"password\":\"nope\"}", "{}", NULL};
  for (int i = 0; i < 3; i++) {
    api_res_t r = th_req(a, "DELETE", "/api/me/mandate", bodies[i], tok, T);
    TEST_ASSERT_EQUAL_INT_MESSAGE(400, r.status, r.body);
    TEST_ASSERT_NOT_NULL(strstr(r.body, "bad_credentials"));
    TEST_ASSERT_NOT_NULL(strstr(r.body, "Das Passwort ist falsch."));
    api_res_free(&r);
  }
  TEST_ASSERT_EQUAL_INT(MANDATE_ACTIVE, db_mandate(&a->db, mid)->status);
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, db_booking(&a->db, r1)->state);
  TEST_ASSERT_TRUE(db_user(&a->db, uid)->active);
  TEST_ASSERT_EQUAL_INT(1, n_sessions_of(uid));
}

void test_revoke_without_mandate_404(void) {
  api_res_t r = th_req(a, "DELETE", "/api/me/mandate", "{\"password\":\"correct horse\"}", tok, T);
  TEST_ASSERT_EQUAL_INT_MESSAGE(404, r.status, r.body);
  TEST_ASSERT_NOT_NULL(strstr(r.body, "not_found"));
  api_res_free(&r);
  TEST_ASSERT_TRUE(db_user(&a->db, uid)->active);
}

void test_revoke_last_admin_409(void) {
  int64_t adm = th_add_user(a, "admin@x.de", "admin pass 1234", ROLE_ADMIN);
  char ta[65];
  login_copy(ta, "admin@x.de", "admin pass 1234");
  const char *body = "{\"password\":\"admin pass 1234\"}";
  api_res_t r = th_req(a, "DELETE", "/api/me/mandate", body, ta, T);
  TEST_ASSERT_EQUAL_INT_MESSAGE(409, r.status, r.body);
  TEST_ASSERT_NOT_NULL(strstr(r.body, "last_admin"));
  api_res_free(&r);
  TEST_ASSERT_TRUE(db_user(&a->db, adm)->active);
  TEST_ASSERT_EQUAL_INT(MANDATE_ACTIVE, db_mandate_current(&a->db, adm)->status);
  int64_t adm2 = th_add_user(a, "admin2@x.de", "admin pass 1234", ROLE_ADMIN);
  TEST_ASSERT_TRUE(adm2 > 0);
  r = th_req(a, "DELETE", "/api/me/mandate", body, ta, T);
  TEST_ASSERT_EQUAL_INT_MESSAGE(200, r.status, r.body);
  api_res_free(&r);
  TEST_ASSERT_FALSE(db_user(&a->db, adm)->active);
}

void test_admin_revoke_other_user(void) {
  int64_t adm = th_add_user_bare(a, "admin@x.de", "admin pass 1234", ROLE_ADMIN);
  char ta[65];
  login_copy(ta, "admin@x.de", "admin pass 1234");
  int64_t mid = add_mandate(uid);
  int64_t act = add_booking(uid, BK_ACTIVE, T - 600);
  int64_t r1 = add_booking(uid, BK_RESERVED, T + 600);
  int64_t r2 = add_booking(uid, BK_RESERVED, T + 86400 * 5);
  char path[64];
  snprintf(path, sizeof path, "/api/admin/users/%lld/mandate/revoke", (long long)uid);
  api_res_t r = th_req(a, "POST", path, NULL, ta, T);
  TEST_ASSERT_EQUAL_INT_MESSAGE(200, r.status, r.body);
  api_res_free(&r);
  check_revoked(uid, adm, mid, r1, act, r2);
  TEST_ASSERT_TRUE(db_user(&a->db, adm)->active);
  r = th_req(a, "POST", path, NULL, ta, T);
  TEST_ASSERT_EQUAL_INT(404, r.status);
  api_res_free(&r);
  snprintf(path, sizeof path, "/api/admin/users/%lld/mandate/revoke", 9999LL);
  r = th_req(a, "POST", path, NULL, ta, T);
  TEST_ASSERT_EQUAL_INT(404, r.status);
  api_res_free(&r);
}

void test_admin_revoke_self_400(void) {
  int64_t adm = th_add_user(a, "admin@x.de", "admin pass 1234", ROLE_ADMIN);
  char ta[65];
  login_copy(ta, "admin@x.de", "admin pass 1234");
  char path[64];
  snprintf(path, sizeof path, "/api/admin/users/%lld/mandate/revoke", (long long)adm);
  api_res_t r = th_req(a, "POST", path, NULL, ta, T);
  TEST_ASSERT_EQUAL_INT_MESSAGE(400, r.status, r.body);
  TEST_ASSERT_NOT_NULL(strstr(r.body, "\"self\""));
  api_res_free(&r);
  TEST_ASSERT_EQUAL_INT(MANDATE_ACTIVE, db_mandate_current(&a->db, adm)->status);
}

void test_admin_mandates_history_newest_first(void) {
  th_add_user_bare(a, "admin@x.de", "admin pass 1234", ROLE_ADMIN);
  char ta[65];
  login_copy(ta, "admin@x.de", "admin pass 1234");
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  cJSON_Delete(do_req("POST", GOOD, 201, NULL));
  cJSON_Delete(do_req("POST",
      "{\"account_holder\":\"Anna Beispiel\",\"iban\":\"DE02 1203 0000 0000 2020 51\",\"creditor_id\":\"DE98ZZZ09999999999\",\"text_version\":1,\"accept\":true}",
      201, NULL));
  char path[64];
  snprintf(path, sizeof path, "/api/admin/users/%lld/mandates", (long long)uid);
  api_res_t r = th_req(a, "GET", path, NULL, ta, T);
  TEST_ASSERT_EQUAL_INT_MESSAGE(200, r.status, r.body);
  TEST_ASSERT_NULL(strstr(r.body, "\"iban\""));
  TEST_ASSERT_NULL(strstr(r.body, "0532013000"));
  cJSON *j = cJSON_Parse(r.body);
  api_res_free(&r);
  TEST_ASSERT_TRUE(cJSON_IsArray(j));
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT(2, cJSON_GetArrayItem(j, 0)->child->valueint);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArrayItem(j, 1)->child->valueint);
  cJSON_Delete(j);
  r = th_req(a, "GET", "/api/admin/users/9999/mandates", NULL, ta, T);
  TEST_ASSERT_EQUAL_INT(404, r.status);
  api_res_free(&r);
  r = th_req(a, "GET", path, NULL, tok, T);
  TEST_ASSERT_EQUAL_INT(403, r.status);
  api_res_free(&r);
}

void test_reactivated_user_has_no_mandate(void) {
  int64_t adm = th_add_user(a, "admin@x.de", "admin pass 1234", ROLE_ADMIN);
  (void)adm;
  char ta[65];
  login_copy(ta, "admin@x.de", "admin pass 1234");
  add_mandate(uid);
  api_res_t r = th_req(a, "DELETE", "/api/me/mandate", "{\"password\":\"correct horse\"}", tok, T);
  TEST_ASSERT_EQUAL_INT(200, r.status);
  api_res_free(&r);
  user_t u = *db_user(&a->db, uid);
  char path[48], body[512];
  snprintf(path, sizeof path, "/api/admin/users/%lld", (long long)uid);
  snprintf(body, sizeof body,
           "{\"email\":\"%s\",\"name\":\"%s\",\"street\":\"\",\"zip\":\"\",\"city\":\"\",\"role\":\"user\",\"active\":true}",
           u.email, u.name);
  r = th_req(a, "PUT", path, body, ta, T);
  TEST_ASSERT_EQUAL_INT_MESSAGE(200, r.status, r.body);
  api_res_free(&r);
  login_copy(tokbuf, "anna@x.de", "correct horse");
  cJSON *j = do_req("GET", NULL, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "mandate")));
  cJSON_Delete(j);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_get_not_ready);
  RUN_TEST(test_get_ready_has_text);
  RUN_TEST(test_post_not_ready_409);
  RUN_TEST(test_post_mandate_normalizes);
  RUN_TEST(test_post_rejects_bad_input);
  RUN_TEST(test_post_stale_text_version_409);
  RUN_TEST(test_post_stale_creditor_409);
  RUN_TEST(test_post_same_data_is_idempotent);
  RUN_TEST(test_post_new_iban_replaces);
  RUN_TEST(test_mandate_keeps_creditor_snapshot);
  RUN_TEST(test_post_mail_failure_still_201);
  RUN_TEST(test_post_heals_second_active_row);
  RUN_TEST(test_revoke_cancels_reserved);
  RUN_TEST(test_revoke_during_trip_409);
  RUN_TEST(test_revoke_wrong_password_400);
  RUN_TEST(test_revoke_without_mandate_404);
  RUN_TEST(test_revoke_last_admin_409);
  RUN_TEST(test_admin_revoke_other_user);
  RUN_TEST(test_admin_revoke_self_400);
  RUN_TEST(test_admin_mandates_history_newest_first);
  RUN_TEST(test_reactivated_user_has_no_mandate);
  return UNITY_END();
}
