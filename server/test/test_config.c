#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static char path[256];

static void write_conf(const char *text) {
  snprintf(path, sizeof path, "%s/ocs_test_conf_XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
  int fd = mkstemp(path);
  TEST_ASSERT_TRUE(fd >= 0);
  FILE *f = fdopen(fd, "w");
  fputs(text, f);
  fclose(f);
}

void test_config_parses_values_and_comments(void) {
  write_conf("# comment\n\n  data_dir = /var/ocs  \nweb_dir=web\r\nbase_url=https://x.de\n"
             "listen=http://0.0.0.0:9\nsmtp_url=smtp://h:25\nsmtp_user=u\nsmtp_pass=p w\n"
             "mail_from=a@x.de\n  # indented comment\n");
  config_t c;
  TEST_ASSERT_EQUAL_INT(0, config_load(path, &c));
  remove(path);
  TEST_ASSERT_EQUAL_STRING("/var/ocs", c.data_dir);
  TEST_ASSERT_EQUAL_STRING("web", c.web_dir);
  TEST_ASSERT_EQUAL_STRING("https://x.de", c.base_url);
  TEST_ASSERT_EQUAL_STRING("http://0.0.0.0:9", c.listen);
  TEST_ASSERT_EQUAL_STRING("smtp://h:25", c.smtp_url);
  TEST_ASSERT_EQUAL_STRING("u", c.smtp_user);
  TEST_ASSERT_EQUAL_STRING("p w", c.smtp_pass);
  TEST_ASSERT_EQUAL_STRING("a@x.de", c.mail_from);
}

void test_config_defaults_listen(void) {
  write_conf("data_dir=d\nweb_dir=w\nbase_url=b\n");
  config_t c;
  TEST_ASSERT_EQUAL_INT(0, config_load(path, &c));
  remove(path);
  TEST_ASSERT_EQUAL_STRING("http://127.0.0.1:8080", c.listen);
  TEST_ASSERT_EQUAL_STRING("", c.smtp_url);
}

void test_config_unknown_key_fails(void) {
  write_conf("data_dir=d\nweb_dir=w\nbase_url=b\nbogus=1\n");
  config_t c;
  TEST_ASSERT_EQUAL_INT(-1, config_load(path, &c));
  remove(path);
}

void test_config_missing_required_fails(void) {
  config_t c;
  write_conf("data_dir=d\nweb_dir=w\n");
  TEST_ASSERT_EQUAL_INT(-1, config_load(path, &c));
  remove(path);
  write_conf("web_dir=w\nbase_url=b\n");
  TEST_ASSERT_EQUAL_INT(-1, config_load(path, &c));
  remove(path);
  TEST_ASSERT_EQUAL_INT(-1, config_load("/nonexistent/ocs.conf", &c));
}

void test_config_too_long_value_fails(void) {
  char big[600];
  memset(big, 'a', sizeof big - 1);
  big[sizeof big - 1] = 0;
  char text[900];
  snprintf(text, sizeof text, "data_dir=%s\nweb_dir=w\nbase_url=b\n", big);
  write_conf(text);
  config_t c;
  TEST_ASSERT_EQUAL_INT(-1, config_load(path, &c));
  remove(path);
  /* exact fit (cap-1) is fine, cap is not */
  char v[129];
  memset(v, 'b', 127);
  v[127] = 0;
  snprintf(text, sizeof text, "data_dir=d\nweb_dir=w\nbase_url=%s\n", v);
  write_conf(text);
  TEST_ASSERT_EQUAL_INT(0, config_load(path, &c));
  remove(path);
  v[127] = 'b';
  v[128] = 0;
  snprintf(text, sizeof text, "data_dir=d\nweb_dir=w\nbase_url=%s\n", v);
  write_conf(text);
  TEST_ASSERT_EQUAL_INT(-1, config_load(path, &c));
  remove(path);
}

void test_config_malformed_line_fails(void) {
  write_conf("data_dir=d\nweb_dir=w\nbase_url=b\njunk line\n");
  config_t c;
  TEST_ASSERT_EQUAL_INT(-1, config_load(path, &c));
  remove(path);
}

void test_tracker_port_parsed(void) {
  write_conf("data_dir=d\nweb_dir=w\nbase_url=b\ntracker_port = 5027\n");
  config_t c;
  TEST_ASSERT_EQUAL_INT(0, config_load(path, &c));
  remove(path);
  TEST_ASSERT_EQUAL_INT(5027, c.tracker_port);
  write_conf("data_dir=d\nweb_dir=w\nbase_url=b\ntracker_port=1\n");
  TEST_ASSERT_EQUAL_INT(0, config_load(path, &c));
  remove(path);
  TEST_ASSERT_EQUAL_INT(1, c.tracker_port);
  write_conf("data_dir=d\nweb_dir=w\nbase_url=b\ntracker_port=65535\n");
  TEST_ASSERT_EQUAL_INT(0, config_load(path, &c));
  remove(path);
  TEST_ASSERT_EQUAL_INT(65535, c.tracker_port);
}

void test_tracker_port_absent_is_zero(void) {
  write_conf("data_dir=d\nweb_dir=w\nbase_url=b\n");
  config_t c;
  TEST_ASSERT_EQUAL_INT(0, config_load(path, &c));
  remove(path);
  TEST_ASSERT_EQUAL_INT(0, c.tracker_port);
}

void test_tracker_port_invalid(void) {
  const char *bad[] = {"0", "70000", "65536", "abc", "", "-1", "+5027", "50 27", "5027x",
                       "99999999999999999999"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    char text[256];
    snprintf(text, sizeof text, "data_dir=d\nweb_dir=w\nbase_url=b\ntracker_port=%s\n", bad[i]);
    write_conf(text);
    config_t c;
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, config_load(path, &c), bad[i]);
    remove(path);
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_config_parses_values_and_comments);
  RUN_TEST(test_config_defaults_listen);
  RUN_TEST(test_config_unknown_key_fails);
  RUN_TEST(test_config_missing_required_fails);
  RUN_TEST(test_config_too_long_value_fails);
  RUN_TEST(test_config_malformed_line_fails);
  RUN_TEST(test_tracker_port_parsed);
  RUN_TEST(test_tracker_port_absent_is_zero);
  RUN_TEST(test_tracker_port_invalid);
  return UNITY_END();
}
