#ifndef OCS_CORE_TELTONIKA_H
#define OCS_CORE_TELTONIKA_H

/* Teltonika Codec 8 Extended (TCP) parser. Pure: no I/O, no allocation.
 * Only timestamp and AVL ID 16 (total odometer) are extracted; positions and
 * other I/O elements are skipped, never kept. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TLT_MAX_DATA_LEN 16384 /* data field length limit (codec id .. number of data 2) */
#define TLT_MAX_RECORDS 255
#define TLT_IO_TOTAL_ODOMETER 16

typedef struct {
  int64_t ts_ms;
  bool has_odometer;
  uint32_t odometer_m;
} tlt_record_t;

/* CRC-16/IBM: reflected poly 0xA001, init 0. */
uint16_t tlt_crc16(const uint8_t *p, size_t n);

/* 1 complete (*used = bytes), 0 need more, -1 invalid. imei gets 15 digits + NUL. */
int tlt_parse_imei(const uint8_t *buf, size_t len, char imei[16], size_t *used);

/* recs must hold TLT_MAX_RECORDS. 1 complete (*n_recs, *used set), 0 need more,
 * -1 invalid. A bad preamble or data length is -1 as soon as it is visible,
 * without waiting for the body. */
int tlt_parse_packet(const uint8_t *buf, size_t len, tlt_record_t *recs, size_t *n_recs,
                     size_t *used);

#endif
