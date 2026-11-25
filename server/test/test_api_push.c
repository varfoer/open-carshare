#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/session.h"
#include "cJSON.h"
#include "core/push.h"
#include "helpers.h"
#include "http/api.h"
#include "http/api_internal.h"
#include "unity.h"

#define T 1774742400 /* 2026-03-29 00:00 UTC */
#define H 3600
#define M 60
#define S (T + 10 * H) /* 12:00 CEST */

#define FCM "https://fcm.googleapis.com/fcm/send/dev1"
#define APPLE "https://web.push.apple.com/dev2"

static app_t *a;
static int64_t u1, u2, car1;
static char tok1[65], tok2[65];

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  u1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  u2 = th_add_user(a, "bert@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(u1 > 0 && u2 > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, T, tok1));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u2, T, tok2));
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "Golf");
  snprintf(c.plate, sizeof c.plate, "KI-G 1");
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  car1 = c.id;
}

void tearDown(void) {
  th_mail_fail(0);
  th_free(a);
}

static cJSON *call(const char *m, const char *p, const char *body, const char *tok, int status) {
  api_res_t r = th_req(a, m, p, body, tok, T);
  TEST_ASSERT_NOT_NULL(r.body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(status, r.status, r.body);
  cJSON *j = cJSON_Parse(r.body);
  api_res_free(&r);
  TEST_ASSERT_NOT_NULL(j);
  return j;
}

static void call_free(const char *m, const char *p, const char *body, const char *tok, int status) {
  cJSON_Delete(call(m, p, body, tok, status));
}

static const char *err_code(const cJSON *j) {
  const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
  return cJSON_IsString(e) ? e->valuestring : "";
}

static void subscribe(const char *tok, const char *endpoint, int status) {
  char body[1400];
  snprintf(body, sizeof body, "{\"endpoint\":\"%s\",\"p256dh\":\"%s\",\"auth\":\"%s\"}", endpoint,
           TH_PUSH_P256DH, TH_PUSH_AUTH);
  call_free("POST", "/api/me/push", body, tok, status);
}

static int subs_of(int64_t user) {
  int n = 0;
  for (size_t i = 0; i < a->db.n_subs; i++) n += a->db.subs[i].user_id == user;
  return n;
}

/* ---- key and subscriptions ---- */

void test_key_is_public_point_and_stable(void) {
  cJSON *j = call("GET", "/api/push/key", NULL, tok1, 200);
  const char *k = cJSON_GetObjectItemCaseSensitive(j, "key")->valuestring;
  uint8_t pub[65];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT(0, b64url_decode(k, pub, sizeof pub, &n));
  TEST_ASSERT_EQUAL_size_t(65, n);
  TEST_ASSERT_EQUAL_HEX8(0x04, pub[0]);
  /* A restart loads the same key from vapid.json. */
  app_t b;
  memset(&b, 0, sizeof b);
  TEST_ASSERT_EQUAL_INT(0, db_load(&b.db, a->cfg.data_dir));
  TEST_ASSERT_EQUAL_INT(0, api_push_init(&b));
  TEST_ASSERT_EQUAL_STRING(k, b.vapid_pub_b64);
  db_free(&b.db);
  cJSON_Delete(j);
  call_free("GET", "/api/push/key", NULL, NULL, 401);
}

void test_subscribe_stores_and_moves_endpoint(void) {
  subscribe(tok1, FCM, 200);
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_subs);
  TEST_ASSERT_EQUAL_INT64(u1, a->db.subs[0].user_id);
  TEST_ASSERT_EQUAL_STRING(TH_PUSH_AUTH, a->db.subs[0].auth);
  subscribe(tok1, FCM, 200); /* same device again: still one row */
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_subs);
  subscribe(tok2, FCM, 200); /* another member logs in on that device */
  TEST_ASSERT_EQUAL_size_t(1, a->db.n_subs);
  TEST_ASSERT_EQUAL_INT64(u2, a->db.subs[0].user_id);
  /* persisted */
  db_t d;
  TEST_ASSERT_EQUAL_INT(0, db_load(&d, a->cfg.data_dir));
  TEST_ASSERT_EQUAL_size_t(1, d.n_subs);
  TEST_ASSERT_EQUAL_STRING(FCM, d.subs[0].endpoint);
  db_free(&d);
}

void test_subscribe_keeps_newest_five(void) {
  char ep[64];
  for (int i = 1; i <= 6; i++) {
    snprintf(ep, sizeof ep, "https://web.push.apple.com/d%d", i);
    subscribe(tok1, ep, 200);
  }
  subscribe(tok2, FCM, 200);
  TEST_ASSERT_EQUAL_INT(5, subs_of(u1));
  TEST_ASSERT_EQUAL_INT(1, subs_of(u2));
  for (size_t i = 0; i < a->db.n_subs; i++)
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a->db.subs[i].endpoint, "https://web.push.apple.com/d1"));
}

void test_subscribe_rejects(void) {
  char body[512];
  cJSON *j;
  /* host outside the allow-list */
  snprintf(body, sizeof body, "{\"endpoint\":\"https://evil.example/x\",\"p256dh\":\"%s\",\"auth\":\"%s\"}",
           TH_PUSH_P256DH, TH_PUSH_AUTH);
  j = call("POST", "/api/me/push", body, tok1, 400);
  TEST_ASSERT_EQUAL_STRING("push_endpoint", err_code(j));
  cJSON_Delete(j);
  /* key of the wrong length, point off the curve, auth of the wrong length, missing field */
  snprintf(body, sizeof body, "{\"endpoint\":\"%s\",\"p256dh\":\"%s\",\"auth\":\"%s\"}", FCM, TH_PUSH_AUTH,
           TH_PUSH_AUTH);
  call_free("POST", "/api/me/push", body, tok1, 400);
  char off[100];
  snprintf(off, sizeof off, "%s", TH_PUSH_P256DH);
  off[60] = off[60] == 'A' ? 'B' : 'A';
  snprintf(body, sizeof body, "{\"endpoint\":\"%s\",\"p256dh\":\"%s\",\"auth\":\"%s\"}", FCM, off, TH_PUSH_AUTH);
  call_free("POST", "/api/me/push", body, tok1, 400);
  snprintf(body, sizeof body, "{\"endpoint\":\"%s\",\"p256dh\":\"%s\",\"auth\":\"AAAA\"}", FCM, TH_PUSH_P256DH);
  call_free("POST", "/api/me/push", body, tok1, 400);
  snprintf(body, sizeof body, "{\"endpoint\":\"%s\",\"p256dh\":\"%s\"}", FCM, TH_PUSH_P256DH);
  call_free("POST", "/api/me/push", body, tok1, 400);
  call_free("POST", "/api/me/push", "{}", NULL, 401);
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_subs);
}

void test_delete_only_own(void) {
  subscribe(tok1, FCM, 200);
  subscribe(tok2, APPLE, 200);
  call_free("DELETE", "/api/me/push", "{\"endpoint\":\"" APPLE "\"}", tok1, 200); /* not anna's */
  TEST_ASSERT_EQUAL_size_t(2, a->db.n_subs);
  call_free("DELETE", "/api/me/push", "{\"endpoint\":\"" FCM "\"}", tok1, 200);
  TEST_ASSERT_EQUAL_INT(0, subs_of(u1));
  TEST_ASSERT_EQUAL_INT(1, subs_of(u2));
  call_free("DELETE", "/api/me/push", "{\"endpoint\":\"" FCM "\"}", tok1, 200); /* already gone */
  call_free("DELETE", "/api/me/push", "{}", tok1, 400);
}

/* ---- sending ---- */

void test_test_push_counts_and_drops_gone(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, APPLE) > 0);
  TEST_ASSERT_TRUE(th_add_push_sub(a, u2, "https://web.push.apple.com/bert") > 0);
  th_push_status_for("dev2", 410);
  cJSON *j = call("POST", "/api/me/push/test", "{}", tok1, 200);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItemCaseSensitive(j, "sent")->valueint);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItemCaseSensitive(j, "failed")->valueint);
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(2, th_push_count()); /* bert's device untouched */
  TEST_ASSERT_EQUAL_INT(1, subs_of(u1));
  for (size_t i = 0; i < a->db.n_subs; i++)
    if (a->db.subs[i].user_id == u1) TEST_ASSERT_EQUAL_STRING(FCM, a->db.subs[i].endpoint);
  /* 500 or no answer: kept */
  th_push_status(500);
  th_push_status_for(NULL, 0);
  j = call("POST", "/api/me/push/test", "{}", tok1, 200);
  TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItemCaseSensitive(j, "sent")->valueint);
  cJSON_Delete(j);
  th_push_status(-1);
  j = call("POST", "/api/me/push/test", "{}", tok1, 200);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItemCaseSensitive(j, "failed")->valueint);
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(1, subs_of(u1));
}

void test_test_push_rate_limited(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  for (int i = 0; i < 5; i++) call_free("POST", "/api/me/push/test", "{}", tok1, 200);
  cJSON *j = call("POST", "/api/me/push/test", "{}", tok1, 429);
  TEST_ASSERT_EQUAL_STRING("rate_limited", err_code(j));
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(5, th_push_count());
  call_free("POST", "/api/me/push/test", "{}", tok2, 200); /* per user */
}

void test_push_request_shape(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, APPLE) > 0);
  char *payload = api_push_payload("Titel", "Text", "/#/buchungen", "booking-1");
  TEST_ASSERT_NOT_NULL(payload);
  TEST_ASSERT_EQUAL_INT(PUSH_SENT, api_push_one(a, &a->db.subs[0], payload, 3600, T));
  TEST_ASSERT_NOT_NULL(strstr(th_push_last_headers(), "TTL: 3600\n"));
  TEST_ASSERT_EQUAL_STRING(APPLE, th_push_last_url());
  TEST_ASSERT_EQUAL_size_t(strlen(payload) + 103, th_push_last_len()); /* 86 header + delimiter + tag */
  free(payload);
  const char *h = th_push_last_auth();
  TEST_ASSERT_EQUAL_INT(0, strncmp(h, "Authorization: vapid t=", 23));
  char k[120];
  snprintf(k, sizeof k, ", k=%s", a->vapid_pub_b64);
  TEST_ASSERT_NOT_NULL(strstr(h, k));
  /* the JWT claims carry the push service origin and the https base_url as contact */
  const char *t = h + 23, *d1 = strchr(t, '.'), *d2 = strchr(d1 + 1, '.');
  char part[600];
  uint8_t raw[600];
  size_t n = 0;
  snprintf(part, sizeof part, "%.*s", (int)(d2 - d1 - 1), d1 + 1);
  TEST_ASSERT_EQUAL_INT(0, b64url_decode(part, raw, sizeof raw - 1, &n));
  raw[n] = 0;
  TEST_ASSERT_NOT_NULL(strstr((const char *)raw, "\"aud\":\"https://web.push.apple.com\""));
  TEST_ASSERT_NOT_NULL(strstr((const char *)raw, "\"sub\":\"" TH_BASE_URL "\""));
}

void test_row_outside_allow_list_is_gone(void) {
  push_sub_t s;
  memset(&s, 0, sizeof s);
  s.user_id = u1;
  snprintf(s.endpoint, sizeof s.endpoint, "https://old-push.example/x");
  snprintf(s.p256dh, sizeof s.p256dh, "%s", TH_PUSH_P256DH);
  snprintf(s.auth, sizeof s.auth, "%s", TH_PUSH_AUTH);
  TEST_ASSERT_EQUAL_INT(0, db_put_push_sub(&a->db, &s));
  TEST_ASSERT_EQUAL_INT(0, api_push_user(a, u1, "{}", 3600, T, NULL, NULL));
  TEST_ASSERT_EQUAL_INT(0, th_push_count());
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_subs);
}

/* ---- reminders by push ---- */

static int64_t plant(booking_state_t st, int64_t start, int64_t end, int64_t actual_start) {
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = car1;
  b.user_id = u1;
  b.booked_start = start;
  b.booked_end = end;
  b.state = st;
  b.created_at = b.modified_at = T;
  b.actual_start = actual_start;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  return b.id;
}

void test_start_reminder_by_push_replaces_mail(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  int64_t id = plant(BK_RESERVED, S, S + 2 * H, 0);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_EQUAL_INT(1, th_push_count());
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->start_reminded);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S - 25 * M));
  TEST_ASSERT_EQUAL_INT(1, th_push_count());
}

void test_reminder_mail_when_push_not_delivered(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  int64_t id = plant(BK_RESERVED, S, S + 2 * H, 0);
  th_push_status(503);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_EQUAL_INT(1, th_push_count());
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
  TEST_ASSERT_EQUAL_STRING("Erinnerung: Fahrt ab 12:00", th_last_mail_subject());
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->start_reminded);
  TEST_ASSERT_EQUAL_INT(1, subs_of(u1)); /* kept: the service may be back next time */
}

void test_reminder_mail_when_device_gone(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  plant(BK_RESERVED, S, S + 2 * H, 0);
  th_push_status(410);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
  TEST_ASSERT_EQUAL_INT(0, subs_of(u1));
}

void test_reminder_push_and_mail_both_fail_retried(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  int64_t id = plant(BK_RESERVED, S, S + 2 * H, 0);
  th_push_status(-1);
  th_mail_fail(1);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->start_reminded);
  th_push_status(201);
  th_mail_fail(0);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 25 * M));
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->start_reminded);
}

void test_push_down_skips_push_for_the_tick(void) {
  int64_t u3 = th_add_user(a, "cora@x.de", NULL, ROLE_USER);
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, APPLE) > 0);
  TEST_ASSERT_TRUE(th_add_push_sub(a, u3, "https://web.push.apple.com/cora") > 0);
  plant(BK_RESERVED, S, S + 2 * H, 0);
  booking_t b = *db_booking(&a->db, plant(BK_RESERVED, S + 3 * H, S + 4 * H, 0));
  b.user_id = u3;
  b.booked_start = S;
  b.car_id = car1 + 100; /* other car: no overlap check here, only reminders */
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  car_t c2;
  memset(&c2, 0, sizeof c2);
  c2.id = car1 + 100;
  snprintf(c2.name, sizeof c2.name, "Polo");
  c2.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c2));
  th_push_status(-1);
  TEST_ASSERT_EQUAL_INT(2, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_EQUAL_INT(1, th_push_count()); /* first device only, then mail only */
  TEST_ASSERT_EQUAL_INT(2, th_mail_count());
  TEST_ASSERT_EQUAL_INT(2, subs_of(u1));     /* kept */
}

void test_rejected_403_is_gone(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  plant(BK_RESERVED, S, S + 2 * H, 0);
  th_push_status(403);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_EQUAL_INT(1, th_mail_count());
  TEST_ASSERT_EQUAL_INT(0, subs_of(u1));
}

void test_start_reminder_ttl_until_start(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  plant(BK_RESERVED, S, S + 2 * H, 0);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S - 30 * M));
  TEST_ASSERT_NOT_NULL(strstr(th_push_last_headers(), "TTL: 1800\n"));
}

void test_endpoint_not_taken_with_other_keys(void) {
  subscribe(tok1, FCM, 200);
  char body[1400];
  snprintf(body, sizeof body, "{\"endpoint\":\"%s\",\"p256dh\":\"%s\",\"auth\":\"AAAAAAAAAAAAAAAAAAAAAA\"}", FCM,
           TH_PUSH_P256DH);
  call_free("POST", "/api/me/push", body, tok2, 400);
  TEST_ASSERT_EQUAL_INT(1, subs_of(u1));
  subscribe(tok2, FCM, 200); /* same browser subscription (same keys): moves */
  TEST_ASSERT_EQUAL_INT(1, subs_of(u2));
}

void test_password_change_drops_push(void) {
  int64_t u4 = th_add_user(a, "dora@x.de", "altes-passwort", ROLE_USER);
  char t4[65];
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u4, T, t4));
  TEST_ASSERT_TRUE(th_add_push_sub(a, u4, FCM) > 0);
  TEST_ASSERT_TRUE(th_add_push_sub(a, u2, APPLE) > 0);
  call_free("PUT", "/api/me/password", "{\"current\":\"altes-passwort\",\"new\":\"neues-passwort\"}", t4, 200);
  TEST_ASSERT_EQUAL_INT(0, subs_of(u4));
  TEST_ASSERT_EQUAL_INT(1, subs_of(u2));
}

void test_password_reset_drops_push(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  TEST_ASSERT_EQUAL_INT(0, api_send_token_mail(a, u1, TOKEN_RESET, T));
  const char *tk = strstr(th_last_mail_body(), "token=");
  TEST_ASSERT_NOT_NULL(tk);
  char token[128], body[256];
  size_t n = 0;
  for (tk += 6; tk[n] && tk[n] != '\n' && tk[n] != '\r' && n < sizeof token - 1; n++) token[n] = tk[n];
  token[n] = 0;
  snprintf(body, sizeof body, "{\"token\":\"%s\",\"password\":\"neues-passwort\"}", token);
  call_free("POST", "/api/password/set", body, NULL, 200);
  TEST_ASSERT_EQUAL_INT(0, subs_of(u1));
}

void test_deactivation_drops_push(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u2, APPLE) > 0);
  int64_t adm = th_add_user(a, "adm@x.de", NULL, ROLE_ADMIN);
  char tadm[65], p[64];
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tadm));
  snprintf(p, sizeof p, "/api/admin/users/%lld", (long long)u2);
  call_free("PUT", p, "{\"email\":\"bert@x.de\",\"name\":\"Bert\",\"street\":\"\",\"zip\":\"\",\"city\":\"\","
                      "\"role\":\"user\",\"active\":false}", tadm, 200);
  TEST_ASSERT_EQUAL_INT(0, subs_of(u2));
}

void test_vapid_sub_from_mail_from(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, APPLE) > 0);
  snprintf(a->cfg.base_url, sizeof a->cfg.base_url, "http://localhost:8080");
  snprintf(a->cfg.mail_from, sizeof a->cfg.mail_from, "Carsharing Verein <noreply@verein.de>");
  TEST_ASSERT_EQUAL_INT(PUSH_SENT, api_push_one(a, &a->db.subs[0], "{}", 600, T));
  const char *h = th_push_last_auth() + 23, *d1 = strchr(h, '.'), *d2 = strchr(d1 + 1, '.');
  char part[600];
  uint8_t raw[600];
  size_t n = 0;
  snprintf(part, sizeof part, "%.*s", (int)(d2 - d1 - 1), d1 + 1);
  TEST_ASSERT_EQUAL_INT(0, b64url_decode(part, raw, sizeof raw - 1, &n));
  raw[n] = 0;
  TEST_ASSERT_NOT_NULL(strstr((const char *)raw, "\"sub\":\"mailto:noreply@verein.de\""));
}

/* ---- end reminder ---- */

void test_end_reminder_mail_in_window(void) {
  int64_t id = plant(BK_ACTIVE, S, S + 2 * H, S);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S + 2 * H - 31 * M));
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S + 2 * H - 30 * M));
  TEST_ASSERT_EQUAL_STRING("Erinnerung: R\xC3\xBC" "ckgabe bis 14:00", th_last_mail_subject());
  TEST_ASSERT_EQUAL_STRING("anna@x.de", th_last_mail_to());
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(), "deine Buchung endet bald."));
  TEST_ASSERT_NULL(strstr(th_last_mail_body(), "Versp\xC3\xA4tungsgeb\xC3\xBChr")); /* no tariff */
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->end_reminded);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S + 2 * H - 5 * M)); /* once */
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S + 2 * H));        /* late reminder still comes */
  TEST_ASSERT_EQUAL_INT(2, th_mail_count());
}

void test_end_reminder_fee_line(void) {
  int64_t t = th_add_tariff(a, "Standard");
  TEST_ASSERT_TRUE(th_add_version(a, t, T - 86400, 0, 0, 500) > 0);
  TEST_ASSERT_TRUE(th_assign_tariff(a, u1, t, T - 86400) > 0);
  plant(BK_ACTIVE, S, S + 2 * H, S);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S + 2 * H - 10 * M));
  TEST_ASSERT_NOT_NULL(strstr(th_last_mail_body(),
                              "Ab 29.03.2026 14:15 kostet jede angefangene Stunde 5,00 \xE2\x82\xAC Versp\xC3\xA4tungsgeb\xC3\xBChr."));
}

void test_end_reminder_by_push(void) {
  TEST_ASSERT_TRUE(th_add_push_sub(a, u1, FCM) > 0);
  int64_t id = plant(BK_ACTIVE, S, S + 2 * H, S);
  TEST_ASSERT_EQUAL_INT(1, api_send_reminders(a, S + 2 * H - 20 * M));
  TEST_ASSERT_EQUAL_INT(1, th_push_count());
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
  TEST_ASSERT_TRUE(db_booking(&a->db, id)->end_reminded);
}

void test_end_reminder_skipped_when_started_in_window(void) {
  int64_t id = plant(BK_ACTIVE, S, S + 2 * H, S + 2 * H - 20 * M);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S + 2 * H - 15 * M));
  TEST_ASSERT_FALSE(db_booking(&a->db, id)->end_reminded);
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
}

void test_end_reminder_off_when_0(void) {
  settings_t s = a->db.settings;
  s.end_reminder_before_min = 0;
  TEST_ASSERT_EQUAL_INT(0, db_put_settings(&a->db, &s));
  plant(BK_ACTIVE, S, S + 2 * H, S);
  TEST_ASSERT_EQUAL_INT(0, api_send_reminders(a, S + 2 * H - 10 * M));
  TEST_ASSERT_EQUAL_INT(0, th_mail_count());
}

void test_end_reminder_rearmed_by_new_end(void) {
  booking_t old;
  memset(&old, 0, sizeof old);
  old.booked_end = S;
  booking_t nb = old;
  nb.end_reminded = nb.late_reminded = true;
  api_booking_changed(&old, &nb);
  TEST_ASSERT_TRUE(nb.end_reminded); /* same end */
  nb.booked_end = S + H;
  api_booking_changed(&old, &nb);
  TEST_ASSERT_FALSE(nb.end_reminded);
  TEST_ASSERT_FALSE(nb.late_reminded);
}

void test_settings_end_reminder(void) {
  int64_t adm = th_add_user(a, "adm@x.de", NULL, ROLE_ADMIN);
  char tadm[65];
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tadm));
  cJSON *j = call("GET", "/api/admin/settings", NULL, tadm, 200);
  TEST_ASSERT_EQUAL_INT(30, cJSON_GetObjectItemCaseSensitive(j, "end_reminder_before_min")->valueint);
  cJSON_ReplaceItemInObject(j, "end_reminder_before_min", cJSON_CreateNumber(45));
  char *body = cJSON_PrintUnformatted(j);
  call_free("PUT", "/api/admin/settings", body, tadm, 200);
  TEST_ASSERT_EQUAL_INT(45, a->db.settings.end_reminder_before_min);
  free(body);
  cJSON_ReplaceItemInObject(j, "end_reminder_before_min", cJSON_CreateNumber(1441));
  body = cJSON_PrintUnformatted(j);
  call_free("PUT", "/api/admin/settings", body, tadm, 400);
  free(body);
  cJSON_DeleteItemFromObject(j, "end_reminder_before_min"); /* optional: value kept */
  body = cJSON_PrintUnformatted(j);
  call_free("PUT", "/api/admin/settings", body, tadm, 200);
  TEST_ASSERT_EQUAL_INT(45, a->db.settings.end_reminder_before_min);
  free(body);
  cJSON_Delete(j);
}

int main(void) {
  if (platform_init() != 0) return 1;
  UNITY_BEGIN();
  RUN_TEST(test_key_is_public_point_and_stable);
  RUN_TEST(test_subscribe_stores_and_moves_endpoint);
  RUN_TEST(test_subscribe_keeps_newest_five);
  RUN_TEST(test_subscribe_rejects);
  RUN_TEST(test_delete_only_own);
  RUN_TEST(test_test_push_counts_and_drops_gone);
  RUN_TEST(test_test_push_rate_limited);
  RUN_TEST(test_push_request_shape);
  RUN_TEST(test_row_outside_allow_list_is_gone);
  RUN_TEST(test_start_reminder_by_push_replaces_mail);
  RUN_TEST(test_reminder_mail_when_push_not_delivered);
  RUN_TEST(test_reminder_mail_when_device_gone);
  RUN_TEST(test_reminder_push_and_mail_both_fail_retried);
  RUN_TEST(test_push_down_skips_push_for_the_tick);
  RUN_TEST(test_rejected_403_is_gone);
  RUN_TEST(test_start_reminder_ttl_until_start);
  RUN_TEST(test_endpoint_not_taken_with_other_keys);
  RUN_TEST(test_password_change_drops_push);
  RUN_TEST(test_password_reset_drops_push);
  RUN_TEST(test_deactivation_drops_push);
  RUN_TEST(test_vapid_sub_from_mail_from);
  RUN_TEST(test_end_reminder_mail_in_window);
  RUN_TEST(test_end_reminder_fee_line);
  RUN_TEST(test_end_reminder_by_push);
  RUN_TEST(test_end_reminder_skipped_when_started_in_window);
  RUN_TEST(test_end_reminder_off_when_0);
  RUN_TEST(test_end_reminder_rearmed_by_new_end);
  RUN_TEST(test_settings_end_reminder);
  return UNITY_END();
}
