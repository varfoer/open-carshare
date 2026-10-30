#include <string.h>

#include "core/mail_build.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define FROM "noreply@x.de"
#define TO "a@x.de"

void test_ascii_subject_plain(void) {
  char b[1024];
  int n = mail_build(b, sizeof b, FROM, TO, "Einladung", "hi", 1793368800);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_INT((int)strlen(b), n);
  TEST_ASSERT_NOT_NULL(strstr(b, "\r\nSubject: Einladung\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "Date: Fri, 30 Oct 2026 14:00:00 +0000\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "From: " FROM "\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "To: " TO "\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "MIME-Version: 1.0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "Content-Type: text/plain; charset=UTF-8\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "Content-Transfer-Encoding: 8bit\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "\r\n\r\nhi\r\n"));
}

void test_umlaut_subject_encoded(void) {
  char b[1024];
  TEST_ASSERT_TRUE(mail_build(b, sizeof b, FROM, TO, "Passwort zur\xC3\xBC" "cksetzen", "x", 0) > 0);
  TEST_ASSERT_NOT_NULL(strstr(b, "Subject: =?UTF-8?B?UGFzc3dvcnQgenVyw7xja3NldHplbg==?=\r\n"));
}

void test_long_utf8_subject_split_at_char_boundary(void) {
  char b[2048], subj[200] = "";
  for (int i = 0; i < 40; i++) strcat(subj, "\xC3\xBC");
  TEST_ASSERT_TRUE(mail_build(b, sizeof b, FROM, TO, subj, "x", 0) > 0);
  TEST_ASSERT_NOT_NULL(strstr(b, "?=\r\n =?UTF-8?B?"));
}

void test_header_injection_rejected(void) {
  char b[1024];
  TEST_ASSERT_EQUAL_INT(-1, mail_build(b, sizeof b, FROM, "a@x.de\r\nBcc: b@y.de", "s", "x", 0));
  TEST_ASSERT_EQUAL_INT(-1, mail_build(b, sizeof b, "a@x.de\nBcc: b@y.de", TO, "s", "x", 0));
  TEST_ASSERT_EQUAL_INT(-1, mail_build(b, sizeof b, FROM, TO, "s\r\nBcc: b@y.de", "x", 0));
}

void test_body_lf_converted_to_crlf(void) {
  char b[1024];
  TEST_ASSERT_TRUE(mail_build(b, sizeof b, FROM, TO, "s", "a\nb\r\nc", 0) > 0);
  TEST_ASSERT_NOT_NULL(strstr(b, "\r\n\r\na\r\nb\r\nc\r\n"));
  TEST_ASSERT_NULL(strstr(b, "\r\r"));
}

void test_buffer_too_small(void) {
  char b[1024];
  TEST_ASSERT_EQUAL_INT(-1, mail_build(b, 10, FROM, TO, "s", "x", 0));
  int n = mail_build(b, sizeof b, FROM, TO, "s", "x", 0);
  TEST_ASSERT_EQUAL_INT(-1, mail_build(b, (size_t)n, FROM, TO, "s", "x", 0));
  TEST_ASSERT_EQUAL_INT(n, mail_build(b, (size_t)n + 1, FROM, TO, "s", "x", 0));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ascii_subject_plain);
  RUN_TEST(test_umlaut_subject_encoded);
  RUN_TEST(test_long_utf8_subject_split_at_char_boundary);
  RUN_TEST(test_header_injection_rejected);
  RUN_TEST(test_body_lf_converted_to_crlf);
  RUN_TEST(test_buffer_too_small);
  return UNITY_END();
}
