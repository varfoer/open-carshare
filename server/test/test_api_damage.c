#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "auth/session.h"
#include "cJSON.h"
#include "helpers.h"
#include "http/api.h"
#include "unity.h"

#define T 1774742400
#define H 3600

static const unsigned char JPG[] = {0xFF, 0xD8, 0xFF, 0xE0, 'J', 'F', 'I', 'F', 0, 0xFF, 0xD9};

static app_t *a;
static int64_t u1, u2, adm, bk;
static char tok1[65], tok2[65], tok_adm[65];

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  /* No password: sessions are created directly, avoiding Argon2id cost. */
  u1 = th_add_user(a, "anna@x.de", NULL, ROLE_USER);
  u2 = th_add_user(a, "bob@x.de", NULL, ROLE_USER);
  adm = th_add_user(a, "admin@x.de", NULL, ROLE_ADMIN);
  TEST_ASSERT_TRUE(u1 > 0 && u2 > 0 && adm > 0);
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u1, T, tok1));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, u2, T, tok2));
  TEST_ASSERT_EQUAL_INT(0, session_create(a, adm, T, tok_adm));
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "Golf");
  snprintf(c.plate, sizeof c.plate, "X-Golf");
  snprintf(c.location, sizeof c.location, "Hof");
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  /* u1's booking, started an hour ago. */
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = c.id;
  b.user_id = u1;
  b.booked_start = T - H;
  b.booked_end = T + H;
  b.state = BK_ACTIVE;
  b.actual_start = T - H;
  b.created_at = b.modified_at = T - 2 * H;
  b.modified_by = u1;
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  bk = b.id;
}

void tearDown(void) { th_free(a); }

static cJSON *parse_res(api_res_t *r, int status, const char *code) {
  TEST_ASSERT_NOT_NULL(r->body);
  TEST_ASSERT_EQUAL_INT_MESSAGE(status, r->status, r->body);
  cJSON *j = cJSON_Parse(r->body);
  api_res_free(r);
  TEST_ASSERT_NOT_NULL(j);
  if (code) {
    cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
    if (!cJSON_IsString(e)) cJSON_Delete(j);
    TEST_ASSERT_TRUE(cJSON_IsString(e));
    TEST_ASSERT_EQUAL_STRING(code, e->valuestring);
  }
  return j;
}

static cJSON *call(const char *m, const char *p, const char *body, const char *tok, int status,
                   const char *code) {
  api_res_t r = th_req(a, m, p, body, tok, T);
  return parse_res(&r, status, code);
}

static void call_free(const char *m, const char *p, const char *body, const char *tok, int status,
                      const char *code) {
  cJSON_Delete(call(m, p, body, tok, status, code));
}

static int64_t num(const cJSON *o, const char *k) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
  TEST_ASSERT_TRUE(cJSON_IsNumber(v));
  return (int64_t)v->valuedouble;
}

static char path_buf[160];
static const char *damage_path(int64_t booking_id) {
  snprintf(path_buf, sizeof path_buf, "/api/bookings/%lld/damage", (long long)booking_id);
  return path_buf;
}
static const char *photos_path(int64_t report_id) {
  snprintf(path_buf, sizeof path_buf, "/api/damage/%lld/photos", (long long)report_id);
  return path_buf;
}
static const char *photo_path(int64_t report_id, const char *k) {
  snprintf(path_buf, sizeof path_buf, "/api/damage/%lld/photos/%s", (long long)report_id, k);
  return path_buf;
}

/* Creates a start-phase report for bk as u1; returns its id. */
static int64_t report(void) {
  cJSON *j = call("POST", damage_path(bk), "{\"phase\":\"start\",\"text\":\"Kratzer vorne links\"}",
                  tok1, 201, NULL);
  int64_t id = num(j, "id");
  cJSON_Delete(j);
  return id;
}

static cJSON *upload(int64_t rid, const void *body, size_t len, const char *tok, const char *ct,
                     int status, const char *code) {
  api_res_t r = th_req_bin(a, "POST", photos_path(rid), body, len, tok, T, ct);
  return parse_res(&r, status, code);
}

static bool photo_file_exists(int64_t rid, int k) {
  char p[600];
  TEST_ASSERT_EQUAL_INT(0, db_photo_path(&a->db, rid, k, p, sizeof p));
  return access(p, F_OK) == 0;
}

void test_report_create_and_list(void) {
  cJSON *j = call("POST", damage_path(bk), "{\"phase\":\"start\",\"text\":\"Kratzer vorne links\"}",
                  tok1, 201, NULL);
  TEST_ASSERT_EQUAL_INT(bk, num(j, "booking_id"));
  TEST_ASSERT_EQUAL_INT(u1, num(j, "user_id"));
  TEST_ASSERT_EQUAL_INT(T, num(j, "created_at"));
  TEST_ASSERT_EQUAL_INT(0, num(j, "n_photos"));
  TEST_ASSERT_EQUAL_STRING("start", cJSON_GetObjectItemCaseSensitive(j, "phase")->valuestring);
  TEST_ASSERT_EQUAL_STRING("Kratzer vorne links",
                           cJSON_GetObjectItemCaseSensitive(j, "text")->valuestring);
  int64_t rid = num(j, "id");
  cJSON_Delete(j);
  j = call("GET", damage_path(bk), NULL, tok1, 200, NULL);
  TEST_ASSERT_TRUE(cJSON_IsArray(j));
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  TEST_ASSERT_EQUAL_INT(rid, num(cJSON_GetArrayItem(j, 0), "id"));
  cJSON_Delete(j);
}

void test_report_text_required(void) {
  call_free("POST", damage_path(bk), "{\"phase\":\"start\",\"text\":\"\"}", tok1, 400, "invalid");
  call_free("POST", damage_path(bk), "{\"phase\":\"start\"}", tok1, 400, "invalid");
  call_free("POST", damage_path(bk), "{\"phase\":\"middle\",\"text\":\"x\"}", tok1, 400, "invalid");
  char *body = malloc(1100);
  TEST_ASSERT_NOT_NULL(body);
  int n = snprintf(body, 1100, "{\"phase\":\"start\",\"text\":\"");
  memset(body + n, 'a', 1024);
  snprintf(body + n + 1024, 1100 - (size_t)n - 1024, "\"}");
  api_res_t r = th_req(a, "POST", damage_path(bk), body, tok1, T);
  free(body);
  cJSON_Delete(parse_res(&r, 400, "invalid"));
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_damages);
}

void test_report_end_needs_return(void) {
  call_free("POST", damage_path(bk), "{\"phase\":\"end\",\"text\":\"Delle\"}", tok1, 400,
            "not_modifiable");
  TEST_ASSERT_EQUAL_size_t(0, a->db.n_damages);
}

void test_report_other_users_booking_404(void) {
  call_free("POST", damage_path(bk), "{\"phase\":\"start\",\"text\":\"x\"}", tok2, 404, "not_found");
  call_free("POST", damage_path(bk), "{\"phase\":\"start\",\"text\":\"x\"}", tok_adm, 404,
            "not_found");
  call_free("POST", damage_path(bk + 100), "{\"phase\":\"start\",\"text\":\"x\"}", tok1, 404,
            "not_found");
}

void test_photo_upload_and_get(void) {
  int64_t rid = report();
  cJSON *j = upload(rid, JPG, sizeof JPG, tok1, "image/jpeg", 201, NULL);
  TEST_ASSERT_EQUAL_INT(rid, num(j, "id"));
  TEST_ASSERT_EQUAL_INT(1, num(j, "n_photos"));
  cJSON_Delete(j);
  TEST_ASSERT_TRUE(photo_file_exists(rid, 1));
  api_res_t r = th_req(a, "GET", photo_path(rid, "1"), NULL, tok1, T);
  TEST_ASSERT_EQUAL_INT(200, r.status);
  TEST_ASSERT_EQUAL_STRING("image/jpeg", r.content_type);
  TEST_ASSERT_EQUAL_size_t(sizeof JPG, r.body_len);
  TEST_ASSERT_EQUAL_MEMORY(JPG, r.body, sizeof JPG);
  api_res_free(&r);
}

void test_photo_rejects_non_jpeg(void) {
  static const unsigned char PNG[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  int64_t rid = report();
  cJSON_Delete(upload(rid, PNG, sizeof PNG, tok1, "image/jpeg", 415, "unsupported_media_type"));
  cJSON_Delete(upload(rid, JPG, 2, tok1, "image/jpeg", 415, "unsupported_media_type"));
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok1, "application/json", 415, NULL));
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok1, NULL, 415, NULL));
  TEST_ASSERT_EQUAL_INT(0, db_damage(&a->db, rid)->n_photos);
  TEST_ASSERT_FALSE(photo_file_exists(rid, 1));
}

void test_photo_too_large_413(void) {
  TEST_ASSERT_EQUAL_size_t(2097152, api_body_limit("POST", "/api/damage/1/photos"));
  TEST_ASSERT_EQUAL_size_t(65536, api_body_limit("POST", "/api/bookings"));
  TEST_ASSERT_EQUAL_size_t(65536, api_body_limit("GET", "/api/damage/1/photos/1"));
  int64_t rid = report();
  size_t n = 2097153;
  unsigned char *big = calloc(n, 1);
  TEST_ASSERT_NOT_NULL(big);
  memcpy(big, JPG, sizeof JPG);
  cJSON_Delete(upload(rid, big, n, tok1, "image/jpeg", 413, "too_large"));
  /* Exactly the limit is accepted. */
  cJSON *j = upload(rid, big, n - 1, tok1, "image/jpeg", 201, NULL);
  free(big);
  TEST_ASSERT_EQUAL_INT(1, num(j, "n_photos"));
  cJSON_Delete(j);
  /* JSON routes keep the 64 KiB limit, also unauthenticated. */
  char *body = malloc(65538);
  TEST_ASSERT_NOT_NULL(body);
  memset(body, ' ', 65537);
  body[65537] = 0;
  api_res_t r = th_req(a, "POST", "/api/bookings", body, NULL, T);
  free(body);
  cJSON_Delete(parse_res(&r, 413, "too_large"));
}

void test_photo_limit_10(void) {
  int64_t rid = report();
  for (int i = 1; i <= 10; i++) {
    cJSON *j = upload(rid, JPG, sizeof JPG, tok1, "image/jpeg", 201, NULL);
    TEST_ASSERT_EQUAL_INT(i, num(j, "n_photos"));
    cJSON_Delete(j);
  }
  cJSON *j = upload(rid, JPG, sizeof JPG, tok1, "image/jpeg", 400, "too_many_photos");
  TEST_ASSERT_EQUAL_STRING("Höchstens 10 Fotos pro Schadensmeldung.",
                           cJSON_GetObjectItemCaseSensitive(j, "message")->valuestring);
  cJSON_Delete(j);
  TEST_ASSERT_EQUAL_INT(10, db_damage(&a->db, rid)->n_photos);
  TEST_ASSERT_TRUE(photo_file_exists(rid, 10));
  TEST_ASSERT_FALSE(photo_file_exists(rid, 11));
}

void test_damage_visible_to_owner_and_admin_only(void) {
  int64_t rid = report();
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok1, "image/jpeg", 201, NULL));
  call_free("GET", damage_path(bk), NULL, tok2, 404, "not_found");
  api_res_t r = th_req(a, "GET", photo_path(rid, "1"), NULL, tok2, T);
  cJSON_Delete(parse_res(&r, 404, "not_found"));
  cJSON *j = call("GET", damage_path(bk), NULL, tok_adm, 200, NULL);
  TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(j));
  cJSON_Delete(j);
  r = th_req(a, "GET", photo_path(rid, "1"), NULL, tok_adm, T);
  TEST_ASSERT_EQUAL_INT(200, r.status);
  TEST_ASSERT_EQUAL_size_t(sizeof JPG, r.body_len);
  api_res_free(&r);
  r = th_req(a, "GET", photo_path(rid, "1"), NULL, NULL, T);
  cJSON_Delete(parse_res(&r, 401, "unauthenticated"));
}

void test_photo_upload_by_admin_404(void) {
  int64_t rid = report();
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok_adm, "image/jpeg", 404, "not_found"));
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok2, "image/jpeg", 404, "not_found"));
  cJSON_Delete(upload(rid + 100, JPG, sizeof JPG, tok1, "image/jpeg", 404, "not_found"));
  TEST_ASSERT_EQUAL_INT(0, db_damage(&a->db, rid)->n_photos);
  TEST_ASSERT_FALSE(photo_file_exists(rid, 1));
}

void test_photo_storage_failure_unlinks(void) {
  int64_t rid = report();
  /* A non-empty directory in place of the table file makes the rename fail. */
  char p[600];
  snprintf(p, sizeof p, "%s/damage_reports.jsonl", a->cfg.data_dir);
  TEST_ASSERT_EQUAL_INT(0, unlink(p));
  TEST_ASSERT_EQUAL_INT(0, mkdir(p, 0700));
  snprintf(p, sizeof p, "%s/damage_reports.jsonl/x", a->cfg.data_dir);
  FILE *f = fopen(p, "w");
  TEST_ASSERT_NOT_NULL(f);
  fclose(f);
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok1, "image/jpeg", 500, "storage"));
  TEST_ASSERT_EQUAL_INT(0, db_damage(&a->db, rid)->n_photos);
  TEST_ASSERT_FALSE(photo_file_exists(rid, 1));
}

void test_photo_index_out_of_range_404(void) {
  int64_t rid = report();
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok1, "image/jpeg", 201, NULL));
  const char *ks[] = {"0", "2", "x", "01x", "-1"};
  for (size_t i = 0; i < sizeof ks / sizeof ks[0]; i++) {
    api_res_t r = th_req(a, "GET", photo_path(rid, ks[i]), NULL, tok1, T);
    cJSON_Delete(parse_res(&r, 404, "not_found"));
  }
}

void test_report_access_follows_booking_owner(void) {
  int64_t rid = report();
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok1, "image/jpeg", 201, NULL));
  booking_t b = *db_booking(&a->db, bk);
  b.user_id = u2; /* admin reassigned the booking */
  TEST_ASSERT_EQUAL_INT(0, db_put_booking(&a->db, &b));
  api_res_t r = th_req(a, "GET", photo_path(rid, "1"), NULL, tok2, T);
  TEST_ASSERT_EQUAL_INT(200, r.status);
  api_res_free(&r);
  r = th_req(a, "GET", photo_path(rid, "1"), NULL, tok1, T);
  cJSON_Delete(parse_res(&r, 404, "not_found"));
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok1, "image/jpeg", 404, "not_found"));
  cJSON_Delete(upload(rid, JPG, sizeof JPG, tok2, "image/jpeg", 201, NULL));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_report_create_and_list);
  RUN_TEST(test_report_text_required);
  RUN_TEST(test_report_end_needs_return);
  RUN_TEST(test_report_other_users_booking_404);
  RUN_TEST(test_photo_upload_and_get);
  RUN_TEST(test_photo_rejects_non_jpeg);
  RUN_TEST(test_photo_too_large_413);
  RUN_TEST(test_photo_limit_10);
  RUN_TEST(test_damage_visible_to_owner_and_admin_only);
  RUN_TEST(test_photo_upload_by_admin_404);
  RUN_TEST(test_photo_storage_failure_unlinks);
  RUN_TEST(test_photo_index_out_of_range_404);
  RUN_TEST(test_report_access_follows_booking_owner);
  return UNITY_END();
}
