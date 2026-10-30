#include <string.h>

#include "auth/password.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void test_hash_verify_roundtrip(void) {
  char h[160];
  TEST_ASSERT_EQUAL_INT(0, pw_hash("correct horse", h));
  TEST_ASSERT_TRUE(pw_verify("correct horse", h));
}

void test_wrong_password_false(void) {
  char h[160];
  TEST_ASSERT_EQUAL_INT(0, pw_hash("correct horse", h));
  TEST_ASSERT_FALSE(pw_verify("wrong horse", h));
}

void test_hash_format_prefix(void) {
  char h[160];
  TEST_ASSERT_EQUAL_INT(0, pw_hash("x", h));
  TEST_ASSERT_EQUAL_INT(0, strncmp(h, "$argon2id$v=19$m=19456,t=2,p=1$", 31));
}

void test_two_hashes_differ(void) {
  char a[160], b[160];
  TEST_ASSERT_EQUAL_INT(0, pw_hash("same", a));
  TEST_ASSERT_EQUAL_INT(0, pw_hash("same", b));
  TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

void test_malformed_encoded_false(void) {
  TEST_ASSERT_FALSE(pw_verify("x", ""));
  TEST_ASSERT_FALSE(pw_verify("x", "$argon2id$"));
  TEST_ASSERT_FALSE(pw_verify("x", "garbage"));
}

void test_hostile_params_rejected(void) {
  /* absurd memory / passes / lanes must be refused, not allocated */
  TEST_ASSERT_FALSE(pw_verify("x", "$argon2id$v=19$m=4294967295,t=2,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  TEST_ASSERT_FALSE(pw_verify("x", "$argon2id$v=19$m=19456,t=4000000000,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  TEST_ASSERT_FALSE(pw_verify("x", "$argon2id$v=19$m=19456,t=2,p=8$AAAAAAAAAAAAAAAAAAAAAA$AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
}

void test_wrong_hash_length_rejected(void) {
  TEST_ASSERT_FALSE(pw_verify("x", "$argon2id$v=19$m=19456,t=2,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAA"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_hash_verify_roundtrip);
  RUN_TEST(test_wrong_password_false);
  RUN_TEST(test_hash_format_prefix);
  RUN_TEST(test_two_hashes_differ);
  RUN_TEST(test_malformed_encoded_false);
  RUN_TEST(test_hostile_params_rejected);
  RUN_TEST(test_wrong_hash_length_rejected);
  return UNITY_END();
}
