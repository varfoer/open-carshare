#include "http/tracker.h"

#include <stdbool.h>

#include "core/teltonika.h"

#define TRACKER_MAX_AGE (90 * 86400) /* oldest accepted reading, by ts */
#define TRACKER_MAX_AHEAD 600        /* tolerated tracker clock lead */
#define TRACKER_KEEPALIVE 0xFF

static int handshake(app_t *a, tracker_conn_t *st, const uint8_t *buf, size_t len,
                     uint8_t *reply, size_t *reply_len, size_t *used) {
  char imei[16];
  size_t n = 0;
  int rc = tlt_parse_imei(buf, len, imei, &n);
  if (rc == 0) return 0;
  const car_t *c = rc == 1 ? db_car_by_imei(&a->db, imei) : NULL;
  reply[0] = c ? 0x01 : 0x00;
  *reply_len = 1;
  if (!c) return -1;
  st->car_id = c->id;
  *used = n;
  return 0;
}

/* Readings from recs worth storing: odometer present, ts plausible, (car_id, ts)
 * neither stored already nor earlier in this packet. Returns their count. */
static size_t collect(const db_t *db, int64_t car_id, const tlt_record_t *recs, size_t n_recs,
                      int64_t now, odometer_reading_t *out) {
  size_t n = 0;
  for (size_t i = 0; i < n_recs; i++) {
    if (!recs[i].has_odometer) continue;
    int64_t ts = recs[i].ts_ms / 1000;
    if (ts < now - TRACKER_MAX_AGE || ts > now + TRACKER_MAX_AHEAD) continue;
    bool dup = false;
    for (size_t k = 0; k < n && !dup; k++) dup = out[k].ts == ts;
    if (dup) continue;
    out[n++] = (odometer_reading_t){
        .car_id = car_id, .ts = ts, .odometer_m = recs[i].odometer_m, .received_at = now};
  }
  /* One pass over the stored readings; drop candidates already present. */
  bool drop[TLT_MAX_RECORDS] = {false};
  for (size_t j = 0; j < db->n_readings; j++) {
    if (db->readings[j].car_id != car_id) continue;
    for (size_t k = 0; k < n; k++)
      if (out[k].ts == db->readings[j].ts) drop[k] = true;
  }
  size_t m = 0;
  for (size_t k = 0; k < n; k++)
    if (!drop[k]) out[m++] = out[k];
  return m;
}

static int packet(app_t *a, tracker_conn_t *st, const uint8_t *buf, size_t len, int64_t now,
                  uint8_t *reply, size_t *reply_len, size_t *used) {
  tlt_record_t recs[TLT_MAX_RECORDS];
  size_t n_recs = 0, n = 0;
  int rc = tlt_parse_packet(buf, len, recs, &n_recs, &n);
  if (rc <= 0) return rc;
  odometer_reading_t rd[TLT_MAX_RECORDS];
  size_t n_rd = collect(&a->db, st->car_id, recs, n_recs, now, rd);
  if (n_rd > 0 && db_add_readings(&a->db, rd, n_rd) != 0) return -1;
  reply[0] = 0;
  reply[1] = 0;
  reply[2] = 0;
  reply[3] = (uint8_t)n_recs; /* n_recs <= TLT_MAX_RECORDS = 255 */
  *reply_len = 4;
  *used = n;
  return 0;
}

int tracker_feed(app_t *a, tracker_conn_t *st, const uint8_t *buf, size_t len, int64_t now,
                 uint8_t *reply, size_t *reply_len, size_t *used) {
  *reply_len = 0;
  *used = 0;
  if (st->car_id == 0) return handshake(a, st, buf, len, reply, reply_len, used);
  if (len == 0) return 0;
  if (buf[0] == TRACKER_KEEPALIVE) {
    *used = 1;
    return 0;
  }
  return packet(a, st, buf, len, now, reply, reply_len, used);
}
