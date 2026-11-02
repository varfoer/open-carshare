#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/teltonika.h"
#include "helpers.h"
#include "http/tracker.h"
#include "unity.h"

#define T INT64_C(1800000000)
#define IMEI "356307042441013"

static app_t *a;
static int64_t car_id;

void setUp(void) {
  a = th_app();
  TEST_ASSERT_NOT_NULL(a);
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "Zoe");
  snprintf(c.plate, sizeof c.plate, "B-OC 1");
  snprintf(c.tracker_imei, sizeof c.tracker_imei, IMEI);
  c.active = true;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &c));
  car_id = c.id;
}

void tearDown(void) { th_free(a); }

static size_t imei_msg(uint8_t *b, const char *digits) {
  b[0] = 0;
  b[1] = 15;
  memcpy(b + 2, digits, 15);
  return 17;
}

typedef struct {
  int rc;
  size_t used, reply_len;
  uint8_t reply[8];
} feed_t;

static feed_t feed(tracker_conn_t *st, const uint8_t *buf, size_t len) {
  feed_t f;
  memset(&f, 0xAB, sizeof f);
  f.rc = tracker_feed(a, st, buf, len, T, f.reply, &f.reply_len, &f.used);
  return f;
}

static void handshake(tracker_conn_t *st) {
  uint8_t b[17];
  size_t n = imei_msg(b, IMEI);
  feed_t f = feed(st, b, n);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  TEST_ASSERT_EQUAL_UINT(17, f.used);
}

static void assert_ack(const feed_t *f, uint32_t n) {
  TEST_ASSERT_EQUAL_UINT(4, f->reply_len);
  uint8_t want[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f->reply, 4);
}

/* 3 records: two with odometer, one without. */
static size_t packet3(uint8_t *out, size_t cap) {
  int64_t ts[3] = {(T - 600) * 1000, (T - 300) * 1000 + 123, (T - 100) * 1000};
  int64_t odo[3] = {1000400, -1, 1012600};
  size_t n = th_tlt_packet(out, cap, ts, odo, 3);
  TEST_ASSERT_TRUE(n > 0);
  return n;
}

static void test_handshake_known_imei(void) {
  tracker_conn_t st = {0};
  uint8_t b[17];
  size_t n = imei_msg(b, IMEI);
  feed_t f = feed(&st, b, n);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  TEST_ASSERT_EQUAL_UINT(17, f.used);
  TEST_ASSERT_EQUAL_UINT(1, f.reply_len);
  TEST_ASSERT_EQUAL_HEX8(0x01, f.reply[0]);
  TEST_ASSERT_EQUAL_INT64(car_id, st.car_id);
}

static void test_handshake_inactive_car_accepted(void) {
  car_t *c = db_car_by_imei(&a->db, IMEI);
  TEST_ASSERT_NOT_NULL(c);
  car_t copy = *c;
  copy.active = false;
  TEST_ASSERT_EQUAL_INT(0, db_put_car(&a->db, &copy));
  tracker_conn_t st = {0};
  uint8_t b[17];
  feed_t f = feed(&st, b, imei_msg(b, IMEI));
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  TEST_ASSERT_EQUAL_UINT(1, f.reply_len);
  TEST_ASSERT_EQUAL_HEX8(0x01, f.reply[0]);
  TEST_ASSERT_EQUAL_INT64(car_id, st.car_id);
}

static void test_unknown_imei_rejected(void) {
  tracker_conn_t st = {0};
  uint8_t b[17];
  feed_t f = feed(&st, b, imei_msg(b, "999999999999999"));
  TEST_ASSERT_EQUAL_INT(-1, f.rc);
  TEST_ASSERT_EQUAL_UINT(1, f.reply_len);
  TEST_ASSERT_EQUAL_HEX8(0x00, f.reply[0]);
  TEST_ASSERT_EQUAL_INT64(0, st.car_id);
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_readings);
}

static void test_imei_need_more(void) {
  tracker_conn_t st = {0};
  uint8_t b[17];
  imei_msg(b, IMEI);
  feed_t f = feed(&st, b, 10);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  TEST_ASSERT_EQUAL_UINT(0, f.used);
  TEST_ASSERT_EQUAL_UINT(0, f.reply_len);
  TEST_ASSERT_EQUAL_INT64(0, st.car_id);
}

static void test_packet_stores_readings_and_acks(void) {
  tracker_conn_t st = {0};
  handshake(&st);
  uint8_t p[1024];
  size_t n = packet3(p, sizeof p);
  feed_t f = feed(&st, p, n);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  TEST_ASSERT_EQUAL_UINT(n, f.used);
  assert_ack(&f, 3);
  TEST_ASSERT_EQUAL_UINT(2, a->db.n_readings);
  TEST_ASSERT_EQUAL_INT64(car_id, a->db.readings[0].car_id);
  TEST_ASSERT_EQUAL_INT64(T - 600, a->db.readings[0].ts);
  TEST_ASSERT_EQUAL_INT64(1000400, a->db.readings[0].odometer_m);
  TEST_ASSERT_EQUAL_INT64(T, a->db.readings[0].received_at);
  TEST_ASSERT_EQUAL_INT64(T - 100, a->db.readings[1].ts);
  TEST_ASSERT_EQUAL_INT64(1012600, a->db.readings[1].odometer_m);
  /* persisted: a reload sees them */
  db_t db2;
  TEST_ASSERT_EQUAL_INT(0, db_load(&db2, a->cfg.data_dir));
  TEST_ASSERT_EQUAL_UINT(2, db2.n_readings);
  db_free(&db2);
}

static void test_packet_split_across_calls(void) {
  tracker_conn_t st = {0};
  handshake(&st);
  uint8_t p[1024];
  size_t n = packet3(p, sizeof p);
  feed_t f = feed(&st, p, n / 2);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  TEST_ASSERT_EQUAL_UINT(0, f.used);
  TEST_ASSERT_EQUAL_UINT(0, f.reply_len);
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_readings);
  f = feed(&st, p, n);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  TEST_ASSERT_EQUAL_UINT(n, f.used);
  assert_ack(&f, 3);
  TEST_ASSERT_EQUAL_UINT(2, a->db.n_readings);
}

static void test_resend_does_not_duplicate(void) {
  tracker_conn_t st = {0};
  handshake(&st);
  uint8_t p[1024];
  size_t n = packet3(p, sizeof p);
  feed_t f = feed(&st, p, n);
  assert_ack(&f, 3);
  f = feed(&st, p, n);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  assert_ack(&f, 3);
  TEST_ASSERT_EQUAL_UINT(2, a->db.n_readings);
}

static void test_duplicate_ts_within_packet_stored_once(void) {
  tracker_conn_t st = {0};
  handshake(&st);
  int64_t ts[2] = {(T - 60) * 1000, (T - 60) * 1000 + 500};
  int64_t odo[2] = {5000, 5000};
  uint8_t p[512];
  size_t n = th_tlt_packet(p, sizeof p, ts, odo, 2);
  feed_t f = feed(&st, p, n);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  assert_ack(&f, 2);
  TEST_ASSERT_EQUAL_UINT(1, a->db.n_readings);
}

static void test_bad_crc_closes_without_ack(void) {
  tracker_conn_t st = {0};
  handshake(&st);
  uint8_t p[1024];
  size_t n = packet3(p, sizeof p);
  p[n - 1] ^= 0xFF;
  feed_t f = feed(&st, p, n);
  TEST_ASSERT_EQUAL_INT(-1, f.rc);
  TEST_ASSERT_EQUAL_UINT(0, f.reply_len);
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_readings);
}

static void test_oversize_length_closes(void) {
  tracker_conn_t st = {0};
  handshake(&st);
  uint32_t dl = TLT_MAX_DATA_LEN + 1;
  uint8_t h[8] = {0, 0, 0, 0, (uint8_t)(dl >> 24), (uint8_t)(dl >> 16), (uint8_t)(dl >> 8), (uint8_t)dl};
  feed_t f = feed(&st, h, sizeof h);
  TEST_ASSERT_EQUAL_INT(-1, f.rc);
  TEST_ASSERT_EQUAL_UINT(0, f.reply_len);
}

static void test_packet_before_handshake_rejected(void) {
  tracker_conn_t st = {0};
  uint8_t p[1024];
  size_t n = packet3(p, sizeof p);
  feed_t f = feed(&st, p, n);
  TEST_ASSERT_EQUAL_INT(-1, f.rc);
  TEST_ASSERT_EQUAL_INT64(0, st.car_id);
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_readings);
}

static void test_keepalive_ff(void) {
  tracker_conn_t st = {0};
  handshake(&st);
  uint8_t b[1] = {0xFF};
  feed_t f = feed(&st, b, 1);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  TEST_ASSERT_EQUAL_UINT(1, f.used);
  TEST_ASSERT_EQUAL_UINT(0, f.reply_len);
}

static void test_empty_input_needs_more(void) {
  tracker_conn_t st = {0};
  handshake(&st);
  feed_t f = feed(&st, (const uint8_t *)"", 0);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  TEST_ASSERT_EQUAL_UINT(0, f.used);
  TEST_ASSERT_EQUAL_UINT(0, f.reply_len);
}

static void test_implausible_timestamp_skipped(void) {
  tracker_conn_t st = {0};
  handshake(&st);
  int64_t ts[4] = {(T + 3600) * 1000, (T + 600) * 1000, (T - 90 * 86400) * 1000,
                   (T - 90 * 86400 - 1) * 1000};
  int64_t odo[4] = {1, 2, 3, 4};
  uint8_t p[1024];
  size_t n = th_tlt_packet(p, sizeof p, ts, odo, 4);
  feed_t f = feed(&st, p, n);
  TEST_ASSERT_EQUAL_INT(0, f.rc);
  assert_ack(&f, 4);
  TEST_ASSERT_EQUAL_UINT(2, a->db.n_readings);
  TEST_ASSERT_EQUAL_INT64(T + 600, a->db.readings[0].ts);
  TEST_ASSERT_EQUAL_INT64(T - 90 * 86400, a->db.readings[1].ts);
}

static void test_storage_failure_no_ack(void) {
  if (geteuid() == 0) TEST_IGNORE_MESSAGE("chmod does not stop root");
  tracker_conn_t st = {0};
  handshake(&st);
  char path[512];
  snprintf(path, sizeof path, "%s/odometer_readings.jsonl", a->cfg.data_dir);
  unlink(path); /* an existing file opened O_APPEND would stay writable */
  TEST_ASSERT_EQUAL_INT(0, chmod(a->cfg.data_dir, 0500));
  uint8_t p[1024];
  size_t n = packet3(p, sizeof p);
  feed_t f = feed(&st, p, n);
  TEST_ASSERT_EQUAL_INT(0, chmod(a->cfg.data_dir, 0700));
  TEST_ASSERT_EQUAL_INT(-1, f.rc);
  TEST_ASSERT_EQUAL_UINT(0, f.reply_len);
  TEST_ASSERT_EQUAL_UINT(0, a->db.n_readings);
}

static void test_expired_before_handshake_after_30s(void) {
  tracker_conn_t st = {.car_id = 0, .last_rx = T + 25, .accepted_at = T};
  TEST_ASSERT_FALSE(tracker_expired(&st, T + 29));
  TEST_ASSERT_FALSE(tracker_expired(&st, T + 30));
  TEST_ASSERT_TRUE(tracker_expired(&st, T + 31)); /* bytes received meanwhile do not help */
}

static void test_expired_after_handshake_by_idle(void) {
  tracker_conn_t st = {.car_id = car_id, .last_rx = T, .accepted_at = T - 5000};
  TEST_ASSERT_FALSE(tracker_expired(&st, T + 899));
  TEST_ASSERT_FALSE(tracker_expired(&st, T + 900));
  TEST_ASSERT_TRUE(tracker_expired(&st, T + 901));
}

static void test_expired_before_handshake_by_idle(void) {
  tracker_conn_t st = {.car_id = 0, .last_rx = T - 1000, .accepted_at = T - 10};
  TEST_ASSERT_TRUE(tracker_expired(&st, T));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_handshake_known_imei);
  RUN_TEST(test_handshake_inactive_car_accepted);
  RUN_TEST(test_unknown_imei_rejected);
  RUN_TEST(test_imei_need_more);
  RUN_TEST(test_packet_stores_readings_and_acks);
  RUN_TEST(test_packet_split_across_calls);
  RUN_TEST(test_resend_does_not_duplicate);
  RUN_TEST(test_duplicate_ts_within_packet_stored_once);
  RUN_TEST(test_bad_crc_closes_without_ack);
  RUN_TEST(test_oversize_length_closes);
  RUN_TEST(test_packet_before_handshake_rejected);
  RUN_TEST(test_keepalive_ff);
  RUN_TEST(test_empty_input_needs_more);
  RUN_TEST(test_implausible_timestamp_skipped);
  RUN_TEST(test_storage_failure_no_ack);
  RUN_TEST(test_expired_before_handshake_after_30s);
  RUN_TEST(test_expired_after_handshake_by_idle);
  RUN_TEST(test_expired_before_handshake_by_idle);
  return UNITY_END();
}
