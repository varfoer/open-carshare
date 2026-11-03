#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/session.h"
#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400 /* 2026-03-29 00:00 UTC */
#define H 3600
#define M 60

static app_t *a;
static int64_t u1, adm, car1;
static char tok1[65], tadm[65];

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  u1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  adm = th_add_user(a, "adm@x.de", NULL, ROLE_ADMIN);
  TEST_ASSERT_TRUE(u1 > 0 && adm > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, T, tok1));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tadm));
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "Golf");
  snprintf(c.plate, sizeof c.plate, "KI-G 1");
  snprintf(c.location, sizeof c.location, "Hof");
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  car1 = c.id;
  settings_t s = a->db.settings;
  s.start_tolerance_min = 15;
  s.cancel_deadline_min = 60;
  s.cancel_fee_cents = 500;
  s.reminder_before_min = 60;
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&a->db, &s));
}

void tearDown(void) {
  th_mail_fail(0);
  th_free(a);
}

static cJSON *call_at(const char *m, const char *p, const char *body, const char *tok, int status, int64_t now) {
  api_res_t r = th_req(a, m, p, body, tok, now);
  TEST_ASSERT_NOT_NULL(r.body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(status, r.status, r.body);
  cJSON *j = cJSON_Parse(r.body);
  api_res_free(&r);
  TEST_ASSERT_NOT_NULL(j);
  return j;
}

static void call_free(const char *m, const char *p, const char *body, const char *tok, int status, int64_t now) {
  cJSON_Delete(call_at(m, p, body, tok, status, now));
}

static int64_t num(const cJSON *o, const char *k) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsNumber(v));
  return (int64_t)v->valuedouble;
}

/* User POST at now; returns the booking id. */
static int64_t user_post(int64_t start, int64_t end, int64_t now) {
  char body[160];
  snprintf(body, sizeof body, "{\"car_id\":%lld,\"start\":%lld,\"end\":%lld}", (long long)car1,
           (long long)start, (long long)end);
  cJSON *j = call_at("POST", "/api/bookings", body, tok1, 201, now);
  int64_t id = num(j, "id");
  cJSON_Delete(j);
  return id;
}

static void user_put(int64_t id, int64_t start, int64_t end, const char *extra, int status, int64_t now) {
  char p[64], body[200];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  snprintf(body, sizeof body, "{\"start\":%lld,\"end\":%lld%s}", (long long)start, (long long)end, extra);
  call_free("PUT", p, body, tok1, status, now);
}

/* Admin PUT with the booked fields of id (start/end overridden when nonzero) plus extra members. */
static void admin_put(int64_t id, int64_t start, int64_t end, const char *extra, int64_t now) {
  const booking_t *b = db_booking(&a->db, id);
  char p[64], body[300];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)id);
  snprintf(body, sizeof body, "{\"car_id\":%lld,\"user_id\":%lld,\"start\":%lld,\"end\":%lld%s}",
           (long long)b->car_id, (long long)b->user_id, (long long)(start ? start : b->booked_start),
           (long long)(end ? end : b->booked_end), extra);
  call_free("PUT", p, body, tadm, 200, now);
}

static void set_flags(int64_t id, bool start_reminded, bool late_reminded) {
  booking_t b = *db_booking(&a->db, id);
  b.start_reminded = start_reminded;
  b.late_reminded = late_reminded;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
}

void test_post_mails_user_created(void) {
  int before = th_mail_count();
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  TEST_ASSERT_EQUAL_INT(before + 1, th_mail_count());
  char subj[64];
  snprintf(subj, sizeof subj, "Buchung %lld best\xC3\xA4tigt", (long long)id);
  TEST_ASSERT_EQUAL_STRING(subj, th_last_mail_subject());
  TEST_ASSERT_EQUAL_STRING("anna@x.de", th_last_mail_to());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "Hallo Test User,"));
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "Auto: Golf (KI-G 1)"));
}

void test_admin_post_mails_member_not_admin(void) {
  char body[200];
  snprintf(body, sizeof body, "{\"car_id\":%lld,\"user_id\":%lld,\"start\":%lld,\"end\":%lld}", (long long)car1,
           (long long)u1, (long long)(T + 2 * H), (long long)(T + 4 * H));
  int before = th_mail_count();
  call_free("POST", "/api/admin/bookings", body, tadm, 201, T);
  TEST_ASSERT_EQUAL_INT(before + 1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("anna@x.de", th_last_mail_to());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_subject(), "best\xC3\xA4tigt"));
}

void test_put_extend_mails_modified(void) {
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  int before = th_mail_count();
  user_put(id, T + 2 * H, T + 5 * H, "", 200, T);
  TEST_ASSERT_EQUAL_INT(before + 1, th_mail_count());
  char subj[64];
  snprintf(subj, sizeof subj, "Buchung %lld ge\xC3\xA4ndert", (long long)id);
  TEST_ASSERT_EQUAL_STRING(subj, th_last_mail_subject());
  TEST_ASSERT_NULL(strstr(th_last_mail_body(), "Vereinsverwaltung"));
}

void test_put_club_flag_only_mails_modified(void) {
  user_t u = *db_user(&a->db, u1);
  u.may_club_trip = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  int before = th_mail_count();
  user_put(id, T + 2 * H, T + 4 * H, ",\"club_trip\":true", 200, T);
  TEST_ASSERT_EQUAL_INT(before + 1, th_mail_count());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "Vereinsfahrt"));
}

void test_admin_put_km_only_no_mail(void) {
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  booking_t b = *db_booking(&a->db, id);
  b.state = BK_DONE;
  b.actual_start = T + 2 * H;
  b.actual_end = T + 4 * H;
  b.km_pending = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  int before = th_mail_count();
  admin_put(id, 0, 0, ",\"km_start\":100,\"km_end\":142", T + 5 * H);
  TEST_ASSERT_EQUAL_INT(before, th_mail_count());
  TEST_ASSERT_EQUAL_INT64(142, db_booking(&a->db, id)->km_end);
}

void test_admin_put_times_mail_says_admin(void) {
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  int before = th_mail_count();
  admin_put(id, T + 3 * H, 0, "", T);
  TEST_ASSERT_EQUAL_INT(before + 1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("anna@x.de", th_last_mail_to());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_subject(), "ge\xC3\xA4ndert"));
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "Vereinsverwaltung"));
}

void test_delete_mails_cancelled_with_fee(void) {
  int64_t id = user_post(T + 30 * M, T + 2 * H, T);
  int before = th_mail_count();
  char p[64];
  snprintf(p, sizeof p, "/api/bookings/%lld", (long long)id);
  call_free("DELETE", p, NULL, tok1, 200, T);
  TEST_ASSERT_EQUAL_INT64(500, db_booking(&a->db, id)->cancel_fee_cents);
  TEST_ASSERT_EQUAL_INT(before + 1, th_mail_count());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_subject(), "storniert"));
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "Stornogeb\xC3\xBChr: 5,00 \xE2\x82\xAC"));
  TEST_ASSERT_NULL(strstr(th_last_mail_body(), "Vereinsverwaltung"));
}

void test_admin_delete_mails_cancelled_by_admin(void) {
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  int before = th_mail_count();
  char p[64];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)id);
  call_free("DELETE", p, NULL, tadm, 200, T);
  TEST_ASSERT_EQUAL_INT(before + 1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("anna@x.de", th_last_mail_to());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "Die Stornierung hat die Vereinsverwaltung vorgenommen."));
}

void test_mail_failure_keeps_201(void) {
  th_mail_fail(1);
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  TEST_ASSERT_NOT_NULL(db_booking(&a->db, id));
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, db_booking(&a->db, id)->state);
}

void test_inactive_user_no_mail(void) {
  /* Ruling: the admin POST for an inactive user is refused (400) by br_check_create, so the
   * skip is exercised on an admin DELETE of a booking whose user is inactive. */
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  user_t u = *db_user(&a->db, u1);
  u.active = false;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
  int before = th_mail_count();
  char p[64];
  snprintf(p, sizeof p, "/api/admin/bookings/%lld", (long long)id);
  call_free("DELETE", p, NULL, tadm, 200, T);
  TEST_ASSERT_EQUAL_INT(BK_CANCELLED, db_booking(&a->db, id)->state);
  TEST_ASSERT_EQUAL_INT(before, th_mail_count());
}

void test_put_start_clears_start_reminded(void) {
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  set_flags(id, true, true);
  user_put(id, T + H, T + 4 * H, "", 200, T);
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->start_reminded);
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->late_reminded);
  set_flags(id, true, true);
  admin_put(id, T + 2 * H, 0, "", T);
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->start_reminded);
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->late_reminded);
}

void test_put_end_clears_late_reminded(void) {
  int64_t id = user_post(T + 2 * H, T + 4 * H, T);
  set_flags(id, true, true);
  user_put(id, T + 2 * H, T + 5 * H, "", 200, T);
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->start_reminded);
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->late_reminded);
  set_flags(id, true, true);
  admin_put(id, 0, T + 6 * H, "", T);
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->start_reminded);
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->late_reminded);
}

/* ---- reminders ---- */

static int64_t plant(booking_state_t st, int64_t start, int64_t end, int64_t created_at) {
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = car1;
  b.user_id = u1;
  b.booked_start = start;
  b.booked_end = end;
  b.state = st;
  b.created_at = b.modified_at = created_at;
  if (st == BK_ACTIVE) b.actual_start = start;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  return b.id;
}

#define S (T + 10 * H)

void test_start_reminder_in_window(void) {
  int64_t id = plant(BK_RESERVED, S, S + 2 * H, T);
  int before = th_mail_count();
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_EQUAL_INT(before + 1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("Erinnerung: Fahrt ab 12:00", th_last_mail_subject()); /* 10:00 UTC, CEST */
  TEST_ASSERT_EQUAL_STRING("anna@x.de", th_last_mail_to());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "Starten kannst du ab 29.03.2026 11:45."));
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->start_reminded);
}

void test_start_reminder_not_before_window(void) {
  int64_t id = plant(BK_RESERVED, S, S + 2 * H, T);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S - 61 * M));
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->start_reminded);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 60 * M));
}

void test_start_reminder_once(void) {
  plant(BK_RESERVED, S, S + 2 * H, T);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S - 25 * M));
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
}

void test_start_reminder_off_when_0(void) {
  settings_t s = a->db.settings;
  s.reminder_before_min = 0;
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&a->db, &s));
  plant(BK_RESERVED, S, S + 2 * H, T);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S - 1 * M));
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S));
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
}

void test_start_reminder_skipped_when_created_in_window(void) {
  plant(BK_RESERVED, S, S + 2 * H, S - 10 * M);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S - 5 * M));
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
}

void test_no_start_reminder_after_start(void) {
  plant(BK_RESERVED, S, S + 2 * H, T);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S));
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S + 10 * M));
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
}

void test_late_reminder_at_end(void) {
  int64_t id = plant(BK_ACTIVE, S, S + 2 * H, T);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S + 2 * H - 1));
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S + 2 * H));
  TEST_ASSERT_EQUAL_STRING("R\xC3\xBC" "ckgabe \xC3\xBC" "berf\xC3\xA4llig: Golf", th_last_mail_subject());
  TEST_ASSERT_NULL(strstr(th_last_mail_body(), "Versp\xC3\xA4tungsgeb\xC3\xBChr")); /* no tariff */
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->late_reminded);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S + 3 * H));
}

void test_late_reminder_fee_from_tariff(void) {
  tariff_t t;
  memset(&t, 0, sizeof t);
  t.car_id = car1;
  t.valid_from = 1;
  t.rate_day_cents_per_h = 300;
  t.rate_night_cents_per_h = 150;
  t.night_start = 1200;
  t.night_end = 360;
  t.late_fee_cents_per_started_h = 300;
  t.created_at = 1;
  TEST_ASSERT_EQUAL_INT(0, db_put_tariff(&a->db, &t));
  plant(BK_ACTIVE, S, S + 2 * H, T);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S + 2 * H));
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "kostet jede angefangene Stunde 3,00 \xE2\x82\xAC"));
}

void test_late_reminder_after_extension_waits_for_new_end(void) {
  int64_t id = plant(BK_ACTIVE, S, S + 2 * H, T);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S + 2 * H));
  user_put(id, S, S + 4 * H, "", 200, S + 2 * H + 30);
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->late_reminded);
  int before = th_mail_count();
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S + 2 * H + M));
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S + 4 * H - 1));
  TEST_ASSERT_EQUAL_INT(before, th_mail_count());
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S + 4 * H));
  TEST_ASSERT_EQUAL_INT(before + 1, th_mail_count());
}

void test_reminder_retried_after_mail_failure_then_once(void) {
  int64_t id = plant(BK_RESERVED, S, S + 2 * H, T);
  th_mail_fail(1);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->start_reminded);
  th_mail_fail(0);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 25 * M));
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->start_reminded);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S - 20 * M));
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
}

void test_reminder_inactive_user_skipped(void) {
  int64_t r = plant(BK_RESERVED, S, S + 2 * H, T);
  int64_t l = plant(BK_ACTIVE, T + H, T + 2 * H, T);
  user_t u = *db_user(&a->db, u1);
  u.active = false;
  TEST_ASSERT_EQUAL_INT(0, db_put_user(&a->db, &u));
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
  TEST_ASSERT_FALSE(db_booking(&a->db, r)->start_reminded);
  TEST_ASSERT_FALSE(db_booking(&a->db, l)->late_reminded);
}

void test_reminder_flag_persisted(void) {
  int64_t r = plant(BK_RESERVED, S, S + 2 * H, T);
  int64_t l = plant(BK_ACTIVE, T + H, T + 2 * H, T);
  TEST_ASSERT_EQUAL_INT(2, api_send_reminders(a, S - 30 * M));
  db_t d;
  TEST_ASSERT_EQUAL_INT(0, db_load(&d, a->cfg.data_dir));
  TEST_ASSERT_TRUE(db_booking(&d, r)->start_reminded);
  TEST_ASSERT_TRUE(db_booking(&d, l)->late_reminded);
  db_free(&d);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_post_mails_user_created);
  RUN_TEST(test_admin_post_mails_member_not_admin);
  RUN_TEST(test_put_extend_mails_modified);
  RUN_TEST(test_put_club_flag_only_mails_modified);
  RUN_TEST(test_admin_put_km_only_no_mail);
  RUN_TEST(test_admin_put_times_mail_says_admin);
  RUN_TEST(test_delete_mails_cancelled_with_fee);
  RUN_TEST(test_admin_delete_mails_cancelled_by_admin);
  RUN_TEST(test_mail_failure_keeps_201);
  RUN_TEST(test_inactive_user_no_mail);
  RUN_TEST(test_put_start_clears_start_reminded);
  RUN_TEST(test_put_end_clears_late_reminded);
  RUN_TEST(test_start_reminder_in_window);
  RUN_TEST(test_start_reminder_not_before_window);
  RUN_TEST(test_start_reminder_once);
  RUN_TEST(test_start_reminder_off_when_0);
  RUN_TEST(test_start_reminder_skipped_when_created_in_window);
  RUN_TEST(test_no_start_reminder_after_start);
  RUN_TEST(test_late_reminder_at_end);
  RUN_TEST(test_late_reminder_fee_from_tariff);
  RUN_TEST(test_late_reminder_after_extension_waits_for_new_end);
  RUN_TEST(test_reminder_retried_after_mail_failure_then_once);
  RUN_TEST(test_reminder_inactive_user_skipped);
  RUN_TEST(test_reminder_flag_persisted);
  return UNITY_END();
}
