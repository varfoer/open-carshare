#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400

static app_t *a;
static int64_t uid;
static char *tok;

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  uid = th_add_user_bare(a, "anna@x.de", "correct horse", ROLE_USER);
  TEST_ASSERT_TRUE(uid > 0);
  tok = th_login_at(a, "anna@x.de", "correct horse", T);
  TEST_ASSERT_NOT_NULL(tok);
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
    "{\"account_holder\":\"Anna Beispiel\",\"iban\":\"de89 3704 0044 0532 0130 00\",\"text_version\":1,\"accept\":true}";

static const char *str(const cJSON *o, const char *k) {
  const cJSON *e = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsString(e));
  return e->valuestring;
}

void test_get_not_ready(void) {
  cJSON *j = do_req("GET", NULL, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "ready")));
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "text")));
  TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(j, "mandate")));
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItemCaseSensitive(j, "text_version")->valueint);
  cJSON_Delete(j);
}

void test_get_ready_has_text(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  cJSON *j = do_req("GET", NULL, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "ready")));
  TEST_ASSERT_NOT_NULL(strstr(str(j, "text"), "Carsharing Testverein e.V."));
  cJSON_Delete(j);
}

void test_post_not_ready_409(void) { post(GOOD, 409, "no_creditor"); }

void test_post_mandate_normalizes(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  cJSON *j = do_req("POST", GOOD, 201, NULL);
  cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "mandate");
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
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013001\",\"text_version\":1,\"accept\":true}", 400, "invalid_iban");
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"bic\":\"XX\",\"text_version\":1,\"accept\":true}", 400, "invalid_bic");
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"text_version\":1,\"accept\":false}", 400, "invalid");
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"text_version\":1}", 400, "invalid");
  post("{\"account_holder\":\"  \",\"iban\":\"DE89370400440532013000\",\"text_version\":1,\"accept\":true}", 400, "invalid");
  char body[400];
  snprintf(body, sizeof body,
           "{\"account_holder\":\"%071d\",\"iban\":\"DE89370400440532013000\",\"text_version\":1,\"accept\":true}", 0);
  post(body, 400, "invalid");
  TEST_ASSERT_EQUAL_INT(0, (int)a->db.n_mandates);
}

void test_post_stale_text_version_409(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  post("{\"account_holder\":\"A\",\"iban\":\"DE89370400440532013000\",\"text_version\":0,\"accept\":true}", 409, "text_changed");
}

void test_post_same_data_is_idempotent(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  post(GOOD, 201, NULL);
  cJSON *j = do_req("POST", GOOD, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j, "mail_sent")));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, (int)a->db.n_mandates);
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
}

void test_post_new_iban_replaces(void) {
  TEST_ASSERT_EQUAL_INT(0, th_set_creditor(a));
  post(GOOD, 201, NULL);
  post("{\"account_holder\":\"Anna Beispiel\",\"iban\":\"GB82WEST12345698765432\",\"text_version\":1,\"accept\":true}", 201, NULL);
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

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_get_not_ready);
  RUN_TEST(test_get_ready_has_text);
  RUN_TEST(test_post_not_ready_409);
  RUN_TEST(test_post_mandate_normalizes);
  RUN_TEST(test_post_rejects_bad_input);
  RUN_TEST(test_post_stale_text_version_409);
  RUN_TEST(test_post_same_data_is_idempotent);
  RUN_TEST(test_post_new_iban_replaces);
  RUN_TEST(test_mandate_keeps_creditor_snapshot);
  RUN_TEST(test_post_mail_failure_still_201);
  RUN_TEST(test_post_heals_second_active_row);
  return UNITY_END();
}
