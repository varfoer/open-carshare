#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "store/db.h"
#include "store/store.h"
#include "unity.h"

static char dir[256];

void setUp(void) {
  const char *base = getenv("TMPDIR");
  snprintf(dir, sizeof dir, "%s/ocs_db_XXXXXX", base && *base ? base : "/tmp");
  TEST_ASSERT_NOT_NULL(mkdtemp(dir));
}

void tearDown(void) {
  chmod(dir, 0700);
  char cmd[300];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  if (system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
}

static user_t mkuser(const char *email) {
  user_t u;
  memset(&u, 0, sizeof u);
  snprintf(u.email, sizeof u.email, "%s", email);
  snprintf(u.name, sizeof u.name, "N");
  u.active = true;
  return u;
}

static booking_t mkbooking(void) {
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = 1; b.user_id = 1; b.booked_start = 100; b.booked_end = 200;
  return b;
}

void test_load_empty_dir(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(0, db.n_users);
  TEST_ASSERT_EQUAL_size_t(0, db.n_cars);
  TEST_ASSERT_EQUAL_size_t(0, db.n_bookings);
  TEST_ASSERT_EQUAL_size_t(0, db.n_sessions);
  TEST_ASSERT_EQUAL_size_t(0, db.n_tokens);
  TEST_ASSERT_EQUAL_INT(15, db.settings.start_tolerance_min);
  db_free(&db);
}

void test_load_dir_too_long(void) {
  char longdir[400];
  memset(longdir, 'a', sizeof longdir - 1);
  longdir[0] = '/';
  longdir[sizeof longdir - 1] = '\0';
  db_t db;
  TEST_ASSERT_EQUAL_INT(-1, db_load(&db, longdir));
}

void test_put_assigns_ids_and_persists(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  user_t a = mkuser("a@x.de"), b = mkuser("b@x.de");
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&db, &a));
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&db, &b));
  TEST_ASSERT_EQUAL_INT64(1, a.id);
  TEST_ASSERT_EQUAL_INT64(2, b.id);
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(2, db.n_users);
  TEST_ASSERT_EQUAL_STRING("a@x.de", db_user(&db, 1)->email);
  TEST_ASSERT_EQUAL_STRING("b@x.de", db_user(&db, 2)->email);
  db_free(&db);
}

void test_put_replaces_existing(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  user_t a = mkuser("a@x.de");
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&db, &a));
  snprintf(a.name, sizeof a.name, "Changed");
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&db, &a));
  TEST_ASSERT_EQUAL_size_t(1, db.n_users);
  TEST_ASSERT_EQUAL_STRING("Changed", db_user(&db, 1)->name);
  db_free(&db);
}

void test_user_by_email_case_insensitive(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  user_t a = mkuser("Anna@x.de");
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&db, &a));
  TEST_ASSERT_NOT_NULL(db_user_by_email(&db, "anna@X.DE"));
  TEST_ASSERT_NULL(db_user_by_email(&db, "bob@x.de"));
  db_free(&db);
}

void test_put_rolls_back_on_write_failure(void) {
  if (geteuid() == 0) TEST_IGNORE_MESSAGE("root ignores directory permissions");
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  booking_t b = mkbooking();
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&db, &b));
  TEST_ASSERT_EQUAL_INT64(1, b.id);
  TEST_ASSERT_EQUAL_INT(0, chmod(dir, 0500));
  booking_t c = mkbooking();
  TEST_ASSERT_EQUAL_INT(-1, db_put_booking(&db, &c));
  TEST_ASSERT_EQUAL_size_t(1, db.n_bookings);
  /* replace failure restores the old element */
  b.booked_end = 999;
  TEST_ASSERT_EQUAL_INT(-1, db_put_booking(&db, &b));
  TEST_ASSERT_EQUAL_INT64(200, db_booking(&db, 1)->booked_end);
  TEST_ASSERT_EQUAL_INT(0, chmod(dir, 0700));
  c = mkbooking();
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&db, &c));
  TEST_ASSERT_EQUAL_INT64(2, c.id);
  db_free(&db);
}

void test_corrupt_file_fails_load(void) {
  char path[300];
  snprintf(path, sizeof path, "%s/users.jsonl", dir);
  FILE *f = fopen(path, "w");
  TEST_ASSERT_NOT_NULL(f);
  fputs("{\n", f);
  fclose(f);
  db_t db;
  TEST_ASSERT_EQUAL_INT(-1, db_load(&db, dir));
}

void test_prune_drops_expired_sessions(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  session_t s1, s2;
  memset(&s1, 0, sizeof s1);
  memset(&s2, 0, sizeof s2);
  strcpy(s1.token_hash, "h1"); s1.user_id = 1; s1.expires_at = 50;
  strcpy(s2.token_hash, "h2"); s2.user_id = 1; s2.expires_at = 500;
  TEST_ASSERT_EQUAL_INT(0, db_put_session(&db, &s1));
  TEST_ASSERT_EQUAL_INT(0, db_put_session(&db, &s2));
  token_t t;
  memset(&t, 0, sizeof t);
  strcpy(t.token_hash, "t1"); t.user_id = 1; t.expires_at = 50;
  TEST_ASSERT_EQUAL_INT(0, db_put_token(&db, &t));
  TEST_ASSERT_EQUAL_INT(0, db_prune(&db, 100));
  TEST_ASSERT_EQUAL_size_t(1, db.n_sessions);
  TEST_ASSERT_EQUAL_STRING("h2", db.sessions[0].token_hash);
  TEST_ASSERT_EQUAL_size_t(0, db.n_tokens);
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(1, db.n_sessions);
  db_free(&db);
}

void test_delete_sessions_and_token_replace(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  session_t s;
  memset(&s, 0, sizeof s);
  strcpy(s.token_hash, "h1"); s.user_id = 1; s.expires_at = 500;
  TEST_ASSERT_EQUAL_INT(0, db_put_session(&db, &s));
  strcpy(s.token_hash, "h2");
  TEST_ASSERT_EQUAL_INT(0, db_put_session(&db, &s));
  TEST_ASSERT_EQUAL_INT(0, db_delete_sessions(&db, "h1", 0));
  TEST_ASSERT_EQUAL_size_t(1, db.n_sessions);
  TEST_ASSERT_EQUAL_INT(0, db_delete_sessions(&db, NULL, 1));
  TEST_ASSERT_EQUAL_size_t(0, db.n_sessions);
  token_t t;
  memset(&t, 0, sizeof t);
  strcpy(t.token_hash, "t1"); t.expires_at = 500;
  TEST_ASSERT_EQUAL_INT(0, db_put_token(&db, &t));
  t.used = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_token(&db, &t));
  TEST_ASSERT_EQUAL_size_t(1, db.n_tokens);
  TEST_ASSERT_TRUE(db_token(&db, "t1")->used);
  db_free(&db);
}

void test_settings_persist(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  settings_t s = {10, 60, 500};
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&db, &s));
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_INT(10, db.settings.start_tolerance_min);
  TEST_ASSERT_EQUAL_INT(60, db.settings.cancel_deadline_min);
  TEST_ASSERT_EQUAL_INT64(500, db.settings.cancel_fee_cents);
  db_free(&db);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_load_empty_dir);
  RUN_TEST(test_load_dir_too_long);
  RUN_TEST(test_put_assigns_ids_and_persists);
  RUN_TEST(test_put_replaces_existing);
  RUN_TEST(test_user_by_email_case_insensitive);
  RUN_TEST(test_put_rolls_back_on_write_failure);
  RUN_TEST(test_corrupt_file_fails_load);
  RUN_TEST(test_prune_drops_expired_sessions);
  RUN_TEST(test_delete_sessions_and_token_replace);
  RUN_TEST(test_settings_persist);
  return UNITY_END();
}
