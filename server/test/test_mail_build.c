#include <stdlib.h>
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

/* C2: the SMTP envelope (MAIL FROM) takes only the address of mail_from. */
void test_envelope_addr(void) {
  char a[64];
  TEST_ASSERT_EQUAL_INT(0, mail_envelope_addr("open-carshare <ocs@example.org>", a, sizeof a));
  TEST_ASSERT_EQUAL_STRING("ocs@example.org", a);
  TEST_ASSERT_EQUAL_INT(0, mail_envelope_addr("ocs@example.org", a, sizeof a));
  TEST_ASSERT_EQUAL_STRING("ocs@example.org", a);
  TEST_ASSERT_EQUAL_INT(0, mail_envelope_addr("  ocs@example.org \t", a, sizeof a));
  TEST_ASSERT_EQUAL_STRING("ocs@example.org", a);
  TEST_ASSERT_EQUAL_INT(0, mail_envelope_addr("\"Car, Share\" < ocs@x.de > ", a, sizeof a));
  TEST_ASSERT_EQUAL_STRING("ocs@x.de", a);
}

void test_envelope_addr_rejects(void) {
  char a[16];
  TEST_ASSERT_EQUAL_INT(-1, mail_envelope_addr("ocs@x.de\r\nRCPT TO:<e@v.il>", a, sizeof a));
  TEST_ASSERT_EQUAL_INT(-1, mail_envelope_addr("Name <ocs@x.de\n>", a, sizeof a));
  TEST_ASSERT_EQUAL_INT(-1, mail_envelope_addr("Name <ocs@x.de", a, sizeof a));
  TEST_ASSERT_EQUAL_INT(-1, mail_envelope_addr("Name <>", a, sizeof a));
  TEST_ASSERT_EQUAL_INT(-1, mail_envelope_addr("   ", a, sizeof a));
  TEST_ASSERT_EQUAL_INT(-1, mail_envelope_addr("Name <a b@x.de>", a, sizeof a));
  TEST_ASSERT_EQUAL_INT(-1, mail_envelope_addr("much-too-long@example.org", a, sizeof a));
  TEST_ASSERT_EQUAL_INT(0, mail_envelope_addr("fits@xx.de", a, sizeof a));
}

void test_att_null_same_as_plain(void) {
  char a[1024], b[1024];
  int n = mail_build(a, sizeof a, FROM, TO, "Einladung", "hi\nthere", 1793368800);
  int m = mail_build_att(b, sizeof b, FROM, TO, "Einladung", "hi\nthere", 1793368800, NULL);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_INT(n, m);
  TEST_ASSERT_EQUAL_STRING(a, b);
  TEST_ASSERT_NULL(strstr(b, "multipart"));
}

void test_att_multipart(void) {
  char b[2048];
  mail_att_t att = {"R-1.pdf", "application/pdf", "%PDF-1.4\n", 9};
  int n = mail_build_att(b, sizeof b, FROM, TO, "Rechnung", "Anbei\n", 0, &att);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_EQUAL_INT((int)strlen(b), n);
  TEST_ASSERT_NOT_NULL(strstr(b, "MIME-Version: 1.0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "Content-Type: multipart/mixed; boundary=\"=_ocs_part_0\"\r\n\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "--=_ocs_part_0\r\nContent-Type: text/plain; charset=UTF-8\r\n"
                                 "Content-Transfer-Encoding: 8bit\r\n\r\nAnbei\r\n--=_ocs_part_0\r\n"));
  TEST_ASSERT_NOT_NULL(strstr(b, "Content-Type: application/pdf\r\nContent-Transfer-Encoding: base64\r\n"
                                 "Content-Disposition: attachment; filename=\"R-1.pdf\"\r\n\r\n"
                                 "JVBERi0xLjQK\r\n--=_ocs_part_0--\r\n"));
  TEST_ASSERT_EQUAL_STRING("--=_ocs_part_0--\r\n", b + n - 18);
}

void test_att_filename_non_ascii_rejected(void) {
  char b[2048];
  mail_att_t att = {"R\xC3\xBC.pdf", "application/pdf", "x", 1};
  TEST_ASSERT_EQUAL_INT(-1, mail_build_att(b, sizeof b, FROM, TO, "s", "x", 0, &att));
  att.filename = "a\"b.pdf";
  TEST_ASSERT_EQUAL_INT(-1, mail_build_att(b, sizeof b, FROM, TO, "s", "x", 0, &att));
  att.filename = "a\r\nBcc: x@y.de";
  TEST_ASSERT_EQUAL_INT(-1, mail_build_att(b, sizeof b, FROM, TO, "s", "x", 0, &att));
}

void test_size_bound_suffices(void) {
  size_t alen = 1024 * 1024, blen = 5000;
  unsigned char *d = malloc(alen);
  char *body = malloc(blen + 1);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_NOT_NULL(body);
  for (size_t i = 0; i < alen; i++) d[i] = (unsigned char)(i * 31);
  memset(body, '\n', blen);
  body[blen] = 0;
  size_t cap = mail_size_bound(blen, alen);
  char *buf = malloc(cap);
  TEST_ASSERT_NOT_NULL(buf);
  mail_att_t att = {"big.bin", "application/octet-stream", d, alen};
  int n = mail_build_att(buf, cap, FROM, TO, "Gro\xC3\x9F", body, 0, &att);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_TRUE((size_t)n < cap);
  free(buf);
  free(body);
  free(d);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ascii_subject_plain);
  RUN_TEST(test_umlaut_subject_encoded);
  RUN_TEST(test_long_utf8_subject_split_at_char_boundary);
  RUN_TEST(test_header_injection_rejected);
  RUN_TEST(test_body_lf_converted_to_crlf);
  RUN_TEST(test_buffer_too_small);
  RUN_TEST(test_envelope_addr);
  RUN_TEST(test_envelope_addr_rejects);
  RUN_TEST(test_att_null_same_as_plain);
  RUN_TEST(test_att_multipart);
  RUN_TEST(test_att_filename_non_ascii_rejected);
  RUN_TEST(test_size_bound_suffices);
  return UNITY_END();
}
