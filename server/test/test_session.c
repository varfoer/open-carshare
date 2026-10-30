#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/session.h"
#include "helpers.h"
#include "unity.h"

#define T 1774742400

static app_t *a;
static int64_t uid;

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  uid = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(uid > 0);
}

void tearDown(void) { th_free(a); }

static char *slurp(const char *name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", a->db.dir, name);
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  static char buf[65536];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  return buf;
}

void test_session_roundtrip(void) {
  char tok[65];
  TEST_ASSERT_EQUAL_INT(0, session_create(a, uid, T, tok));
  TEST_ASSERT_EQUAL_size_t(64, strlen(tok));
  user_t *u = session_user(a, tok, T + 10);
  TEST_ASSERT_NOT_NULL(u);
  TEST_ASSERT_EQUAL_INT64(uid, u->id);
  TEST_ASSERT_NULL(session_user(a, "nope", T));
  TEST_ASSERT_NULL(session_user(a, NULL, T));
  TEST_ASSERT_EQUAL_INT(0, session_destroy(a, tok));
  TEST_ASSERT_NULL(session_user(a, tok, T + 10));
}

void test_session_tokens_differ(void) {
  char t1[65], t2[65];
  TEST_ASSERT_EQUAL_INT(0, session_create(a, uid, T, t1));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, uid, T, t2));
  TEST_ASSERT_TRUE(strcmp(t1, t2) != 0);
}

void test_session_expired_after_30_days(void) {
  char tok[65];
  TEST_ASSERT_EQUAL_INT(0, session_create(a, uid, T, tok));
  TEST_ASSERT_NOT_NULL(session_user(a, tok, T + 2592000 - 1));
  TEST_ASSERT_NULL(session_user(a, tok, T + 2592000));
}

void test_session_inactive_user_rejected(void) {
  char tok[65];
  TEST_ASSERT_EQUAL_INT(0, session_create(a, uid, T, tok));
  user_t u = *db_user(&a->db, uid);
  u.active = false;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
  TEST_ASSERT_NULL(session_user(a, tok, T));
}

void test_session_stored_hashed(void) {
  char tok[65], h[65];
  TEST_ASSERT_EQUAL_INT(0, session_create(a, uid, T, tok));
  char *s = slurp("sessions.jsonl");
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_NULL(strstr(s, tok));
  session_hash(tok, h);
  TEST_ASSERT_NOT_NULL(strstr(s, h));
}

void test_token_single_use(void) {
  char tok[65];
  TEST_ASSERT_EQUAL_INT(0, token_issue(a, uid, TOKEN_RESET, T, tok));
  TEST_ASSERT_EQUAL_INT64(uid, token_consume(a, tok, T + 1));
  TEST_ASSERT_EQUAL_INT64(-1, token_consume(a, tok, T + 2));
  char *s = slurp("tokens.jsonl");
  TEST_ASSERT_NOT_NULL(s);
  TEST_ASSERT_NULL(strstr(s, tok));
}

void test_token_unknown_rejected(void) {
  TEST_ASSERT_EQUAL_INT64(-1, token_consume(a, "x", T));
  TEST_ASSERT_EQUAL_INT64(-1, token_consume(a, NULL, T));
  char tok[65];
  memset(tok, 'a', 64);
  tok[64] = 0;
  TEST_ASSERT_EQUAL_INT64(-1, token_consume(a, tok, T));
}

void test_reset_token_expires_after_1h(void) {
  char tok[65];
  TEST_ASSERT_EQUAL_INT(0, token_issue(a, uid, TOKEN_RESET, T, tok));
  TEST_ASSERT_EQUAL_INT64(-1, token_consume(a, tok, T + 3601));
  TEST_ASSERT_EQUAL_INT(0, token_issue(a, uid, TOKEN_RESET, T, tok));
  TEST_ASSERT_EQUAL_INT64(uid, token_consume(a, tok, T + 3599));
}

void test_invite_token_valid_7_days(void) {
  char tok[65];
  TEST_ASSERT_EQUAL_INT(0, token_issue(a, uid, TOKEN_INVITE, T, tok));
  TEST_ASSERT_EQUAL_INT64(uid, token_consume(a, tok, T + 604799));
  TEST_ASSERT_EQUAL_INT(0, token_issue(a, uid, TOKEN_INVITE, T, tok));
  TEST_ASSERT_EQUAL_INT64(-1, token_consume(a, tok, T + 604800));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_session_roundtrip);
  RUN_TEST(test_session_tokens_differ);
  RUN_TEST(test_session_expired_after_30_days);
  RUN_TEST(test_session_inactive_user_rejected);
  RUN_TEST(test_session_stored_hashed);
  RUN_TEST(test_token_single_use);
  RUN_TEST(test_token_unknown_rejected);
  RUN_TEST(test_reset_token_expires_after_1h);
  RUN_TEST(test_invite_token_valid_7_days);
  return UNITY_END();
}
