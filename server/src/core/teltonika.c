#include "core/teltonika.h"

#include <string.h>

#define CODEC_8E 0x8E

uint16_t tlt_crc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

int tlt_parse_imei(const uint8_t *buf, size_t len, char imei[16], size_t *used) {
  if (len >= 2 && (buf[0] != 0 || buf[1] != 15)) return -1;
  for (size_t i = 2; i < len && i < 17; i++)
    if (buf[i] < '0' || buf[i] > '9') return -1;
  if (len < 17) return 0;
  memcpy(imei, buf + 2, 15);
  imei[15] = 0;
  *used = 17;
  return 1;
}

/* Bounds-checked big-endian reader over the data field. */
typedef struct {
  const uint8_t *p;
  size_t len, pos;
} rd_t;

static bool rd_skip(rd_t *r, size_t n) {
  if (n > r->len - r->pos) return false;
  r->pos += n;
  return true;
}

static bool rd_uint(rd_t *r, size_t n, uint64_t *v) {
  if (n > r->len - r->pos) return false;
  uint64_t x = 0;
  for (size_t i = 0; i < n; i++) x = (x << 8) | r->p[r->pos + i];
  r->pos += n;
  *v = x;
  return true;
}

/* One group of `count` elements of id(2)+value(vsz); sets *odo for IO 16 when vsz == 4. */
static bool rd_group(rd_t *r, size_t vsz, tlt_record_t *rec) {
  uint64_t count;
  if (!rd_uint(r, 2, &count)) return false;
  size_t el = 2 + vsz;
  if (count > (r->len - r->pos) / el) return false;
  for (uint64_t i = 0; i < count; i++) {
    uint64_t id, v;
    rd_uint(r, 2, &id);
    rd_uint(r, vsz, &v);
    if (vsz == 4 && id == TLT_IO_TOTAL_ODOMETER) {
      rec->has_odometer = true;
      rec->odometer_m = (uint32_t)v;
    }
  }
  return true;
}

static bool rd_record(rd_t *r, tlt_record_t *rec) {
  uint64_t ts, skip;
  memset(rec, 0, sizeof *rec);
  if (!rd_uint(r, 8, &ts)) return false;
  if (ts > (uint64_t)INT64_MAX) return false;
  rec->ts_ms = (int64_t)ts;
  if (!rd_skip(r, 1 + 15)) return false;   /* priority, GPS */
  if (!rd_uint(r, 2, &skip)) return false; /* event IO id */
  if (!rd_uint(r, 2, &skip)) return false; /* total IO count */
  if (!rd_group(r, 1, rec) || !rd_group(r, 2, rec) || !rd_group(r, 4, rec) || !rd_group(r, 8, rec))
    return false;
  uint64_t nx;
  if (!rd_uint(r, 2, &nx)) return false;
  for (uint64_t i = 0; i < nx; i++) {
    uint64_t id, vl;
    if (!rd_uint(r, 2, &id) || !rd_uint(r, 2, &vl) || !rd_skip(r, (size_t)vl)) return false;
  }
  return true;
}

int tlt_parse_packet(const uint8_t *buf, size_t len, tlt_record_t *recs, size_t *n_recs,
                     size_t *used) {
  for (size_t i = 0; i < len && i < 4; i++)
    if (buf[i] != 0) return -1;
  if (len < 8) return 0;
  size_t dlen = ((size_t)buf[4] << 24) | ((size_t)buf[5] << 16) | ((size_t)buf[6] << 8) | buf[7];
  if (dlen < 3 || dlen > TLT_MAX_DATA_LEN) return -1;
  size_t total = 8 + dlen + 4;
  if (len < total) return 0;
  const uint8_t *d = buf + 8;
  if (d[dlen] != 0 || d[dlen + 1] != 0) return -1;
  uint16_t crc = (uint16_t)((d[dlen + 2] << 8) | d[dlen + 3]);
  if (crc != tlt_crc16(d, dlen)) return -1;
  if (d[0] != CODEC_8E) return -1;
  size_t n = d[1];
  if (n == 0 || d[dlen - 1] != n) return -1;
  rd_t r = {.p = d, .len = dlen - 1, .pos = 2}; /* last byte is N2 */
  for (size_t i = 0; i < n; i++)
    if (!rd_record(&r, &recs[i])) return -1;
  if (r.pos != r.len) return -1;
  *n_recs = n;
  *used = total;
  return 1;
}
