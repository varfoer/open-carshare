/* libFuzzer target for the Codec 8 Extended parser and the tracker session. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/teltonika.h"
#include "helpers.h"
#include "http/tracker.h"

#define IMEI "356307042441013"
#define NOW INT64_C(1800000000)
#define MAX_INPUT (1 << 20)

static app_t *g_app;

static void cleanup(void) { th_free(g_app); }

static void setup(void) {
  g_app = th_app();
  assert(g_app);
  atexit(cleanup);
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "Fuzz");
  snprintf(c.tracker_imei, sizeof c.tracker_imei, IMEI);
  c.active = true;
  if (db_put_car(&g_app->db, &c) != 0) abort();
}

/* Feeds buf like the server's read loop does; returns when it closes or needs more. */
static void feed_all(tracker_conn_t *st, const uint8_t *buf, size_t len) {
  while (len > 0) {
    uint8_t reply[4];
    size_t reply_len = 99, used = 99;
    int rc = tracker_feed(g_app, st, buf, len, NOW, reply, &reply_len, &used);
    assert(reply_len <= sizeof reply);
    assert(used <= len);
    if (rc < 0) return;
    assert(rc == 0);
    if (used == 0) return;
    buf += used;
    len -= used;
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char imei[16];
  tlt_record_t recs[TLT_MAX_RECORDS];
  size_t used = 0, n = 0;
  if (tlt_parse_imei(data, size, imei, &used) == 1) assert(used <= size);
  used = 0;
  if (tlt_parse_packet(data, size, recs, &n, &used) == 1) {
    assert(used <= size);
    assert(n <= TLT_MAX_RECORDS);
  }

  if (size > MAX_INPUT) return 0;
  if (!g_app) setup();
  /* Raw input as the first message: the handshake path. */
  tracker_conn_t st = {0, 0};
  feed_all(&st, data, size);
  /* IMEI message first, so the input reaches the packet path. */
  static uint8_t buf[17 + MAX_INPUT];
  buf[0] = 0;
  buf[1] = 15;
  memcpy(buf + 2, IMEI, 15);
  if (size > 0) memcpy(buf + 17, data, size);
  memset(&st, 0, sizeof st);
  feed_all(&st, buf, 17 + size);
  assert(st.car_id != 0);
  /* Keep the readings table (memory and file) small across runs. */
  if (g_app->db.n_readings > 1000 && db_prune_readings(&g_app->db, INT64_MAX) != 0) abort();
  return 0;
}
