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

/* C1 second line of defence: values the codec cannot read back are never written. */
void test_put_rejects_unsafe_int(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  booking_t b = mkbooking();
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&db, &b));
  booking_t big = b;
  big.booked_end = 4323455642275676160LL;
  TEST_ASSERT_EQUAL_INT(-1, db_put_booking(&db, &big));
  TEST_ASSERT_EQUAL_INT64(200, db_booking(&db, b.id)->booked_end);
  booking_t nb = mkbooking();
  nb.booked_start = -9007199254740993LL; /* -(2^53 + 1) */
  TEST_ASSERT_EQUAL_INT(-1, db_put_booking(&db, &nb));
  TEST_ASSERT_EQUAL_size_t(1, db.n_bookings);
  user_t u = mkuser("big@x.de");
  u.license_checked_at = 9007199254740993LL; /* 2^53 + 1 */
  TEST_ASSERT_EQUAL_INT(-1, db_put_user(&db, &u));
  TEST_ASSERT_EQUAL_size_t(0, db.n_users);
  /* 2^53 would print as 9007199254740990 (cJSON "%1.15g"): refused too */
  u.license_checked_at = 9007199254740992LL;
  TEST_ASSERT_EQUAL_INT(-1, db_put_user(&db, &u));
  /* 10^15 - 1 is the largest written value and loads back unchanged */
  u.license_checked_at = 999999999999999LL;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&db, &u));
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(1, db.n_bookings);
  TEST_ASSERT_EQUAL_INT64(200, db.bookings[0].booked_end);
  TEST_ASSERT_EQUAL_INT64(999999999999999LL, db.users[0].license_checked_at);
  db_free(&db);
}

void test_damage_put_and_reload(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  damage_report_t d;
  memset(&d, 0, sizeof d);
  d.booking_id = 5; d.user_id = 2; d.phase = DMG_START; d.created_at = 99; d.n_photos = 1;
  snprintf(d.text, sizeof d.text, "Delle");
  TEST_ASSERT_EQUAL_INT(0, db_put_damage(&db, &d));
  TEST_ASSERT_EQUAL_INT64(1, d.id);
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(1, db.n_damages);
  damage_report_t *r = db_damage(&db, 1);
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_INT64(5, r->booking_id);
  TEST_ASSERT_EQUAL_STRING("Delle", r->text);
  TEST_ASSERT_EQUAL_INT(1, r->n_photos);
  db_free(&db);
}

void test_load_creates_photos_dir(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  char p[300];
  snprintf(p, sizeof p, "%s/photos", dir);
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
  TEST_ASSERT_TRUE(S_ISDIR(st.st_mode));
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir)); /* existing dir is fine */
  db_free(&db);
}

void test_photo_path(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  char p[300];
  TEST_ASSERT_EQUAL_INT(0, db_photo_path(&db, 7, 2, p, sizeof p));
  size_t n = strlen(p);
  TEST_ASSERT_TRUE(n > 16 && strcmp(p + n - 16, "/photos/r7-2.jpg") == 0);
  char small[8];
  TEST_ASSERT_EQUAL_INT(-1, db_photo_path(&db, 7, 2, small, sizeof small));
  db_free(&db);
}

void test_store_read_file_limit(void) {
  char p[300];
  snprintf(p, sizeof p, "%s/f.bin", dir);
  FILE *f = fopen(p, "wb");
  TEST_ASSERT_NOT_NULL(f);
  fwrite("0123456789", 1, 10, f);
  fclose(f);
  char *buf = NULL;
  size_t len = 0;
  TEST_ASSERT_EQUAL_INT(0, store_read_file(p, 10, &buf, &len));
  TEST_ASSERT_EQUAL_size_t(10, len);
  TEST_ASSERT_EQUAL_MEMORY("0123456789", buf, 10);
  free(buf);
  TEST_ASSERT_EQUAL_INT(-1, store_read_file(p, 9, &buf, &len));
  snprintf(p, sizeof p, "%s/missing", dir);
  TEST_ASSERT_EQUAL_INT(-1, store_read_file(p, 10, &buf, &len));
}

static void rpath(char *p, size_t cap) { snprintf(p, cap, "%s/odometer_readings.jsonl", dir); }

static void write_raw(const char *p, const char *mode, const char *data) {
  FILE *f = fopen(p, mode);
  TEST_ASSERT_NOT_NULL(f);
  fputs(data, f);
  fclose(f);
}

static odometer_reading_t mkreading(int64_t car, int64_t ts, int64_t m) {
  odometer_reading_t r = {car, ts, m, ts + 1};
  return r;
}

void test_readings_append_and_reload(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  odometer_reading_t a[2] = {mkreading(1, 10, 1000), mkreading(1, 20, 2000)};
  odometer_reading_t b = mkreading(2, 30, 3000);
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, a, 2));
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, &b, 1));
  TEST_ASSERT_EQUAL_size_t(3, db.n_readings);
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(3, db.n_readings);
  TEST_ASSERT_EQUAL_INT64(10, db.readings[0].ts);
  TEST_ASSERT_EQUAL_INT64(20, db.readings[1].ts);
  TEST_ASSERT_EQUAL_INT64(3000, db.readings[2].odometer_m);
  TEST_ASSERT_EQUAL_INT64(2, db.readings[2].car_id);
  db_free(&db);
}

void test_readings_load_skips_torn_last_line(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  odometer_reading_t a = mkreading(1, 10, 1000);
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, &a, 1));
  db_free(&db);
  char p[300];
  rpath(p, sizeof p);
  write_raw(p, "ab", "{\"car_id\":1,\"ts\":");
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(1, db.n_readings);
  db_free(&db);
}

void test_readings_append_after_torn_line_reloads(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  odometer_reading_t a = mkreading(1, 10, 1000), b = mkreading(1, 20, 2000);
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, &a, 1));
  db_free(&db);
  char p[300];
  rpath(p, sizeof p);
  write_raw(p, "ab", "{\"car_id\":1,\"ts\":");
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, &b, 1));
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(2, db.n_readings);
  TEST_ASSERT_EQUAL_INT64(20, db.readings[1].ts);
  db_free(&db);
}

void test_readings_unterminated_valid_last_line_is_dropped(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  odometer_reading_t a = mkreading(1, 10, 1000), b = mkreading(1, 30, 3000);
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, &a, 1));
  db_free(&db);
  char p[300];
  rpath(p, sizeof p);
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
  off_t good = st.st_size;
  write_raw(p, "ab", "{\"car_id\":1,\"ts\":20,\"odometer_m\":2000,\"received_at\":21}");
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(1, db.n_readings);
  TEST_ASSERT_EQUAL_INT(0, stat(p, &st));
  TEST_ASSERT_EQUAL_INT64((int64_t)good, (int64_t)st.st_size);
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, &b, 1));
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(2, db.n_readings);
  TEST_ASSERT_EQUAL_INT64(30, db.readings[1].ts);
  db_free(&db);
}

void test_readings_unterminated_invalid_reading_is_dropped(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  db_free(&db);
  char p[300];
  rpath(p, sizeof p);
  write_raw(p, "wb", "{\"car_id\":1}");
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(0, db.n_readings);
  db_free(&db);
}

void test_readings_corrupt_middle_line_fails_load(void) {
  odometer_reading_t a = mkreading(1, 10, 1000);
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, &a, 1));
  db_free(&db);
  char p[300];
  rpath(p, sizeof p);
  write_raw(p, "wb", "{\"car_id\":1,\"ts\":\n");
  write_raw(p, "ab", "{\"car_id\":1,\"ts\":10,\"odometer_m\":1000,\"received_at\":11}\n");
  TEST_ASSERT_EQUAL_INT(-1, db_load(&db, dir));
}

void test_readings_prune(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  odometer_reading_t a[3] = {mkreading(1, 100, 1), mkreading(1, 200, 2), mkreading(1, 300, 3)};
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, a, 3));
  TEST_ASSERT_EQUAL_INT(0, db_prune_readings(&db, 200));
  TEST_ASSERT_EQUAL_size_t(2, db.n_readings);
  TEST_ASSERT_EQUAL_INT64(200, db.readings[0].ts);
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_size_t(2, db.n_readings);
  TEST_ASSERT_EQUAL_INT64(300, db.readings[1].ts);
  db_free(&db);
}

void test_reading_line_has_only_four_keys(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  odometer_reading_t a = mkreading(1, 10, 1000);
  TEST_ASSERT_EQUAL_INT(0, db_add_readings(&db, &a, 1));
  db_free(&db);
  char p[300];
  rpath(p, sizeof p);
  char *buf = NULL;
  size_t len = 0;
  TEST_ASSERT_EQUAL_INT(0, store_read_file(p, 4096, &buf, &len));
  cJSON *j = cJSON_Parse(buf);
  free(buf);
  TEST_ASSERT_NOT_NULL(j);
  TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(j));
  cJSON_Delete(j);
}

void test_car_by_imei(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  car_t a, b;
  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  snprintf(a.name, sizeof a.name, "A");
  snprintf(b.name, sizeof b.name, "B");
  snprintf(b.tracker_imei, sizeof b.tracker_imei, "123456789012345");
  a.active = true;
  b.active = false;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&db, &a));
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&db, &b));
  car_t *f = db_car_by_imei(&db, "123456789012345");
  TEST_ASSERT_NOT_NULL(f);
  TEST_ASSERT_EQUAL_INT64(b.id, f->id);
  TEST_ASSERT_NULL(db_car_by_imei(&db, ""));
  TEST_ASSERT_NULL(db_car_by_imei(&db, "999999999999999"));
  db_free(&db);
}

void test_km_mail_day_persists(void) {
  db_t db;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_INT64(0, db.km_mail_day);
  TEST_ASSERT_EQUAL_INT(0, db_put_km_mail_day(&db, 20500));
  TEST_ASSERT_EQUAL_INT64(20500, db.km_mail_day);
  db_free(&db);
  TEST_ASSERT_EQUAL_INT(0, db_load(&db, dir));
  TEST_ASSERT_EQUAL_INT64(20500, db.km_mail_day);
  db_free(&db);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_load_empty_dir);
  RUN_TEST(test_load_dir_too_long);
  RUN_TEST(test_put_assigns_ids_and_persists);
  RUN_TEST(test_put_replaces_existing);
  RUN_TEST(test_put_rejects_unsafe_int);
  RUN_TEST(test_user_by_email_case_insensitive);
  RUN_TEST(test_put_rolls_back_on_write_failure);
  RUN_TEST(test_corrupt_file_fails_load);
  RUN_TEST(test_prune_drops_expired_sessions);
  RUN_TEST(test_delete_sessions_and_token_replace);
  RUN_TEST(test_settings_persist);
  RUN_TEST(test_damage_put_and_reload);
  RUN_TEST(test_load_creates_photos_dir);
  RUN_TEST(test_photo_path);
  RUN_TEST(test_store_read_file_limit);
  RUN_TEST(test_readings_append_and_reload);
  RUN_TEST(test_readings_load_skips_torn_last_line);
  RUN_TEST(test_readings_append_after_torn_line_reloads);
  RUN_TEST(test_readings_unterminated_valid_last_line_is_dropped);
  RUN_TEST(test_readings_unterminated_invalid_reading_is_dropped);
  RUN_TEST(test_readings_corrupt_middle_line_fails_load);
  RUN_TEST(test_readings_prune);
  RUN_TEST(test_reading_line_has_only_four_keys);
  RUN_TEST(test_car_by_imei);
  RUN_TEST(test_km_mail_day_persists);
  return UNITY_END();
}
