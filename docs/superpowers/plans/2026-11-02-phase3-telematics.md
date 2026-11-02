# Phase 3: Odometer (Teltonika Telematics) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Returned bookings get km automatically from Teltonika trackers that send directly to `ocs-server`; admin can still override manually.

**Architecture:** A pure Codec 8 Extended parser (`core/teltonika.c`) and a pure km resolver (`core/km_resolve.c`) carry the logic. `http/tracker.c` turns received bytes into stored odometer readings and replies, without sockets, so tests feed it bytes directly; `http/server.c` only wires a second Mongoose listener to it. Readings are an append-only JSONL table held in RAM. A 5-minute timer resolves `km_pending` bookings and sends the daily admin mail.

**Tech Stack:** unchanged: C11, CMake, Mongoose 7.14, cJSON, Unity, vanilla JS (ES2020), `node --test`.

**Spec:** `docs/spec.md` §4 (car, booking, odometer_reading), §8, §10, §11, §15 Phase 3; `docs/backlog.md`.

**Branch:** `phase3-telematics` (spec commit `ffd9597` is already on it).

**Product-owner decisions (2026-11-02):** Teltonika 4G OBD tracker, direct ingestion, no Traccar, no map, GNSS odometer accepted for billing (few percent off the dashboard). Hardware is not bought yet; the hardware acceptance (one tracker in the Leapmotor T03 for a week) is outside this plan.

**Out of scope:** TLS on the tracker port (unverified on the firmware), OEM odometer (AVL ID 389), Codec 8 (`0x08`) and UDP, prices (Phase 4).

## Global Constraints

- All Phase 1 and Phase 2 Global Constraints still hold (`int64_t` UTC seconds, no float, German UI, English code, fixed `char[]` reject-not-truncate, `-Wall -Wextra -Werror`, ASan/UBSan tests, db copy-before-modify, every changed web file in `web/sw.js` `SHELL` and `CACHE` bumped once per task that changes web files).
- Data written by Phase 2 must still load: `tracker_imei` optional on car load; `km_source` `"geotab"` on load maps to `telematics` (never written).
- `tracker_imei`: `""` (no tracker) or exactly 15 ASCII digits; unique among cars.
- From tracker data only `car_id`, `ts`, `odometer_m`, `received_at` are stored. Positions and other I/O elements are never stored or logged.
- Readings older than 90 days (`90 * 86400` s by `ts`) are pruned.
- Odometer element: AVL ID `16` (4-byte, metres). Codec ID `0x8E` only.
- km values stay in `[0, 10000000]`; `km_start = round(start_m / 1000)`, `km_end = km_start + round((end_m - start_m) / 1000)`, half-up in integers (`(m + 500) / 1000`).
- `tracker_port` (config file): integer `1..65535`; absent = listener disabled. Binds `0.0.0.0`.
- Admin mail: German, to all active admins, at most one per UTC day (`now / 86400`), only for tracker cars with a booking `km_pending` and `now - actual_end > 86400`.

## Review Focus

1. **Dead-zone buffer arrives late and out of order:** records for the end of a trip arrive after a newer parked record → km still use the last reading inside the trip. Test: Task 3 `test_resolve_out_of_order_arrival`.
2. **Tracker resends a packet whose ACK was lost:** the same records arrive twice → no duplicate readings, km unchanged. Test: Task 5 `test_resend_does_not_duplicate`.
3. **Crash during append:** `odometer_readings.jsonl` ends in a torn line → server still starts, torn line skipped. Test: Task 2 `test_readings_load_skips_torn_last_line`.
4. **Hostile or broken client on the open port:** unknown IMEI, bad CRC, absurd length → connection closed, nothing stored, no ACK. Tests: Task 5 `test_unknown_imei_rejected`, `test_bad_crc_closes_without_ack`, `test_oversize_length_closes`.
5. **Admin enters km during the trip, then the user returns:** km stay manual, not pending, the job never overwrites them. Test: Task 4 `test_return_keeps_manual_km`.

---

## File Structure

```
server/src/core/model.h             km_source_t KM_TELEMATICS, car_t.tracker_imei, odometer_reading_t
server/src/core/codec.{h,c}         car tracker_imei, km_source names, odometer_reading codec
server/src/store/store.{h,c}        + store_append, store_read_log
server/src/store/db.{h,c}           + readings table (append), prune, car by IMEI, km mail day (jobs.json)
server/src/core/teltonika.{h,c}     NEW: Codec 8 Extended parser, CRC-16/IBM
server/src/core/km_resolve.{h,c}    NEW: km from readings for one trip
server/src/core/booking_rules.{h,c} br_apply_trip(old, nb, now)
server/src/http/api_booking.c       return keeps manual km
server/src/http/api_admin.c         trip edit passes old; car tracker_imei
server/src/http/tracker.{h,c}       NEW: bytes in -> readings + reply
server/src/http/api_km.c            NEW: api_resolve_km, api_km_pending_mail
server/src/http/api.h               declarations of the two jobs
server/src/http/server.c            tracker listener, km timer, readings prune
server/src/app.h, config.c          tracker_port
server/test/helpers.{h,c}           + th_tlt_packet
server/test/test_teltonika.c        NEW
server/test/test_km_resolve.c       NEW
server/test/test_tracker.c          NEW
server/test/test_api_km.c           NEW
server/fuzz/fuzz_teltonika.c        NEW
web/js/views/admin_cars.js          IMEI field
web/js/views/admin_bookings.js      "Tracker" suffix on km
deploy/vps/README.md, ocs.conf.example   port, firewall, Teltonika configuration
```

---

### Task 1: Model and codec

**Files:**
- Modify: `server/src/core/model.h`, `server/src/core/codec.h`, `server/src/core/codec.c`
- Test: `server/test/test_codec.c`

**Interfaces:**
- Produces:
  - `typedef enum { KM_NONE = 0, KM_TELEMATICS, KM_MANUAL } km_source_t;` (replaces `KM_GEOTAB`)
  - `car_t` gains `char tracker_imei[16];` (`""` = no tracker)
  - `typedef struct { int64_t car_id, ts, odometer_m, received_at; } odometer_reading_t;`
  - `cJSON *odometer_reading_to_json(const odometer_reading_t *v);`
  - `int odometer_reading_from_json(const cJSON *j, odometer_reading_t *out);` (all four keys required)
  - `bool imei_valid(const char *s);` in `codec.h`: true for exactly 15 ASCII digits.

- [ ] **Step 1: Write the failing tests** in `test_codec.c`:
  - `test_car_tracker_imei_roundtrip`: car with `tracker_imei = "356307042441013"` → to_json → from_json equal; key `"tracker_imei"` present.
  - `test_car_loads_without_tracker_imei`: Phase 2 line `{"id":1,"name":"a","plate":"b","location":"c","active":true}` loads, `tracker_imei` is `""`.
  - `test_car_rejects_bad_imei`: `"tracker_imei":"12345"` and `"35630704244101x"` → `-1`.
  - `test_km_source_telematics_roundtrip`: `KM_TELEMATICS` writes `"telematics"` and loads back.
  - `test_km_source_geotab_loads_as_telematics`: booking line with `"km_source":"geotab"` loads as `KM_TELEMATICS`.
  - `test_odometer_reading_roundtrip`: `{car_id 2, ts 1774742400, odometer_m 22949000, received_at 1774742410}` round-trips; the JSON object has exactly 4 keys (`cJSON_GetArraySize == 4`).
  - `test_odometer_reading_requires_all_keys`: each of the four keys missing → `-1`.
- [ ] **Step 2: Run** `cmake --build build && ctest --test-dir build -R test_codec --output-on-failure` — Expected: compile errors.
- [ ] **Step 3: Implement.** `KM_SOURCE_NAMES = {"none", "telematics", "manual"}`; on load, before the enum lookup, map the string `"geotab"` to `KM_TELEMATICS`. `tracker_imei` optional on load (absent → `""`), present must be `""` or `imei_valid`. Fix every `KM_GEOTAB` use (`grep -rn KM_GEOTAB server`).
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — Expected: all pass.
- [ ] **Step 5: Commit** `feat(core): tracker IMEI, telematics km source, odometer reading codec`

---

### Task 2: Readings store

**Files:**
- Modify: `server/src/store/store.{h,c}`, `server/src/store/db.{h,c}`
- Test: `server/test/test_db.c`

**Interfaces:**
- Consumes: Task 1 `odometer_reading_t`, codec.
- Produces:
  - `int store_append(const char *path, const char *data, size_t len);` — `open(O_WRONLY|O_APPEND|O_CREAT, 0600)`, write all, `fsync`. On any failure `ftruncate` back to the size before the call and return `-1`.
  - `int store_read_log(const char *path, int (*cb)(const cJSON *, void *), void *ctx);` — like `store_read_lines`, but a final line without trailing `'\n'` that does not parse is skipped (torn append) instead of failing.
  - `db_t` gains `odometer_reading_t *readings; size_t n_readings, cap_readings; int64_t km_mail_day;`
  - `car_t *db_car_by_imei(db_t *db, const char *imei);` — NULL for `""` or no match; matches active and inactive cars.
  - `int db_add_readings(db_t *db, const odometer_reading_t *r, size_t n);` — serialize all `n` lines, one `store_append` to `odometer_readings.jsonl`, then append to RAM. `-1`: nothing added (RAM unchanged).
  - `int db_prune_readings(db_t *db, int64_t before);` — drop `ts < before`, rewrite the file with `store_write_atomic`; RAM restored on failure.
  - `int db_put_km_mail_day(db_t *db, int64_t day);` — writes `jobs.json` as `{"km_mail_day":N}` atomically, then sets `db->km_mail_day`. `db_load` reads it (absent file or key → 0).

- [ ] **Step 1: Write the failing tests** in `test_db.c`:
  - `test_readings_append_and_reload`: add 2 then 1 reading, `db_free`, `db_load` → 3 readings in insertion order.
  - `test_readings_load_skips_torn_last_line`: append the bytes `{"car_id":1,"ts":` without newline to `odometer_readings.jsonl` after one valid line → `db_load` returns 0 with 1 reading.
  - `test_readings_corrupt_middle_line_fails_load`: an unparsable line followed by a valid line → `db_load` returns `-1`.
  - `test_readings_prune`: readings with ts 100, 200, 300; `db_prune_readings(db, 200)` → 2 remain in RAM and after reload.
  - `test_reading_line_has_only_four_keys`: after `db_add_readings`, the file's line parses to an object with exactly 4 keys.
  - `test_car_by_imei`: two cars, one inactive with IMEI → found; `""` → NULL; unknown → NULL.
  - `test_km_mail_day_persists`: `db_put_km_mail_day(db, 20500)` → reload → `km_mail_day == 20500`; fresh dir → 0.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_db --output-on-failure` — Expected: compile errors / FAIL.
- [ ] **Step 3: Implement** the interfaces above; load `odometer_readings.jsonl` with `store_read_log` in `db_load`, free in `db_free`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — Expected: all pass.
- [ ] **Step 5: Commit** `feat(store): append-only odometer readings, km mail day`

---

### Task 3: Pure logic — Codec 8 Extended parser and km resolver

**Files:**
- Create: `server/src/core/teltonika.{h,c}`, `server/src/core/km_resolve.{h,c}`, `server/test/test_teltonika.c`, `server/test/test_km_resolve.c`, `server/fuzz/fuzz_teltonika.c`
- Modify: `server/CMakeLists.txt` (`ocs_add_test(test_teltonika)`, `ocs_add_test(test_km_resolve)`, `fuzz_teltonika` in the `OCS_FUZZ` block like `fuzz_api`), `server/test/helpers.{h,c}`

**Interfaces:**
- Produces (`core/teltonika.h`):
  ```c
  #define TLT_MAX_DATA_LEN 16384     /* data field length limit (codec id .. number of data 2) */
  #define TLT_MAX_RECORDS 255
  #define TLT_IO_TOTAL_ODOMETER 16
  typedef struct { int64_t ts_ms; bool has_odometer; uint32_t odometer_m; } tlt_record_t;
  uint16_t tlt_crc16(const uint8_t *p, size_t n);  /* CRC-16/IBM: reflected poly 0xA001, init 0 */
  /* 1 complete (*used = bytes), 0 need more, -1 invalid. imei gets 15 digits + NUL. */
  int tlt_parse_imei(const uint8_t *buf, size_t len, char imei[16], size_t *used);
  /* recs must hold TLT_MAX_RECORDS. 1 complete (*n_recs, *used set), 0 need more, -1 invalid. */
  int tlt_parse_packet(const uint8_t *buf, size_t len, tlt_record_t *recs, size_t *n_recs, size_t *used);
  ```
- Produces (`core/km_resolve.h`):
  ```c
  typedef enum { KMR_OK = 0, KMR_WAIT, KMR_BAD } kmr_result_t;
  kmr_result_t km_resolve(const odometer_reading_t *r, size_t n, int64_t car_id,
                          int64_t actual_start, int64_t actual_end,
                          int64_t *km_start, int64_t *km_end);
  ```
- Produces (`test/helpers.h`): `size_t th_tlt_packet(uint8_t *out, size_t cap, const int64_t *ts_ms, const int64_t *odo_m, size_t n);` — a valid Codec 8E packet with `n` records, zero GPS element, each record carrying only IO 16 = `odo_m[i]` as N4 (`odo_m[i] < 0`: no I/O at all). Returns length, 0 if it does not fit.

Wire format (Codec 8 Extended, TCP): IMEI message = 2-byte big-endian length (must be 15) + 15 ASCII digits. Packet = 4 zero bytes, 4-byte data length, codec id `0x8E`, N1 (1 byte), N1 records, N2 (1 byte, must equal N1), 4-byte CRC (upper 2 bytes zero, lower 2 = `tlt_crc16` over codec id .. N2). Record = timestamp ms (8), priority (1), GPS (15), event IO id (2), total IO count (2), then four groups N1/N2/N4/N8 each `count(2)` + `count × (id(2) + value(1|2|4|8))`, then NX `count(2)` + `count × (id(2) + len(2) + len bytes)`. All big-endian. Invalid: non-zero preamble, length `< 3` or `> TLT_MAX_DATA_LEN`, codec ≠ `0x8E`, N1 = 0, N1 ≠ N2, a record running past the data field or bytes left over, CRC mismatch.

km_resolve: consider only readings of `car_id`. `KMR_WAIT` if none has `ts > actual_end`. Window = `actual_start <= ts <= actual_end`; start = smallest `ts` in window, end = largest (ties: either, they are duplicates). Empty window: last reading before `actual_start` and first after `actual_end` with equal `odometer_m` → `KMR_OK` with that value at both ends, otherwise `KMR_BAD`. `end_m < start_m` or `km_end > 10000000` → `KMR_BAD`. Rounding per Global Constraints.

- [ ] **Step 1: Write the failing tests.** `test_teltonika.c`:
  - `test_wiki_example_packet`: bytes `00000000 0000004A 8E 01 0000016B412CEE00 01` + 15 zero bytes + `0001 0005 0001 0001 01 0001 0011 001D 0001 0010 015E2C88 0002 000B 000000003544C87A 000E 000000001DD7E06A 0000 01 00002994` → returns 1, `used == 86`, `n_recs == 1`, `ts_ms == 1560166592000`, `has_odometer`, `odometer_m == 22949000`. (This is the Codec 8 Extended example from the Teltonika wiki, "Teltonika Data Sending Protocols"; length 74 and CRC `0x2994` were checked.)
  - `test_crc16_wiki_example`: `tlt_crc16` over the 74 data bytes above == `0x2994`.
  - `test_packet_need_more`: every prefix of the example (length 0..85) → 0.
  - `test_packet_bad_crc`, `test_packet_bad_codec` (`0x08`), `test_packet_n1_ne_n2`, `test_packet_length_too_big` (`TLT_MAX_DATA_LEN + 1`), `test_packet_nonzero_preamble` → -1.
  - `test_packet_record_without_odometer`: `th_tlt_packet` with `odo_m = -1` → `has_odometer == false`.
  - `test_packet_nx_group_skipped`: a record with one NX element (id 0x0100, len 3) and IO 16 → odometer read correctly.
  - `test_imei_ok`, `test_imei_need_more`, `test_imei_bad_len` (length field 14), `test_imei_non_digit` → 1 / 0 / -1 / -1.
  
  `test_km_resolve.c` (start S, end E, readings of car 1 unless noted):
  - `test_resolve_basic`: readings S+10: 1000400, E-10: 1012600, E+3600: 1012600 → OK, `km_start 1000`, `km_end 1012` (12.2 km rounds to 12).
  - `test_resolve_rounds_trip_once`: start_m 1000499, end_m 1001500 → `km_start 1000`, `km_end 1001` (trip 1001 m → 1 km).
  - `test_resolve_wait_without_later_reading`: no reading with `ts > E` → WAIT.
  - `test_resolve_out_of_order_arrival`: array order E+3600, S+10, E-10, E-300 → same result as sorted.
  - `test_resolve_zero_km_parked`: readings S-100: 5000000, E+100: 5000000 → OK, 5000/5000.
  - `test_resolve_gap_bad`: readings S-100: 5000000, E+100: 5003000, none inside → BAD.
  - `test_resolve_backwards_bad`: inside readings 2000000 then 1000000 → BAD.
  - `test_resolve_ignores_other_car`: car 2 readings inside the window do not count.
- [ ] **Step 2: Run** `cmake --build build && ctest --test-dir build -R 'test_teltonika|test_km_resolve' --output-on-failure` — Expected: compile errors.
- [ ] **Step 3: Implement** both modules (no I/O, no allocation) and `th_tlt_packet`.
- [ ] **Step 4: Write `fuzz/fuzz_teltonika.c`**: input passed to `tlt_parse_imei` and `tlt_parse_packet`; assert `used <= len` and `n_recs <= TLT_MAX_RECORDS` on 1.
- [ ] **Step 5: Run** `ctest --test-dir build --output-on-failure` — all pass; then `cmake --build build-fuzz --target fuzz_teltonika && ./build-fuzz/server/fuzz_teltonika -max_total_time=60` (`build-fuzz` is configured with `CC=clang -DOCS_FUZZ=ON`) — Expected: no crash.
- [ ] **Step 6: Commit** `feat(core): Codec 8 Extended parser and km resolution`

---

### Task 4: Trip rules keep manual km, telematics km follow time edits

**Files:**
- Modify: `server/src/core/booking_rules.{h,c}`, `server/src/http/api_booking.c` (`h_booking_return`), `server/src/http/api_admin.c` (`h_admin_booking_put`)
- Test: `server/test/test_booking_rules.c`, `server/test/test_api_booking.c`, `server/test/test_api_admin.c`

**Interfaces:**
- Produces: `br_result_t br_apply_trip(const booking_t *old, booking_t *nb, int64_t now);` — as before, plus: when the result is DONE, `nb->km_source == KM_TELEMATICS` and `actual_start` or `actual_end` differs from `old`, set `km_source = KM_NONE`, `km_start = km_end = 0`. Then `km_pending = km_source == KM_NONE` as today. (Manual km in the same PUT were already set to `KM_MANUAL` by `read_trip_fields` and are kept.)
- `h_booking_return`: `b.km_pending = b.km_source == KM_NONE;`

- [ ] **Step 1: Write the failing tests:**
  - `test_booking_rules.c` `test_apply_trip_resets_telematics_on_time_change`: old DONE telematics 1000/1012, nb with `actual_end` moved → `km_source KM_NONE`, km 0/0, `km_pending` true.
  - `test_apply_trip_keeps_telematics_if_times_same` → unchanged, not pending.
  - `test_apply_trip_keeps_manual_on_time_change` → manual km kept, not pending.
  - `test_api_booking.c` `test_return_keeps_manual_km`: active booking with `km_source = KM_MANUAL`, km 100/150 (set via admin PUT) → user return → 200, `km_pending` false, `km_source` `"manual"`, km unchanged.
  - `test_api_admin.c` `test_admin_time_edit_reopens_telematics_km`: DONE telematics booking, admin PUT with new `actual_end` → response `km_pending` true, `km_source` `"none"`.
- [ ] **Step 2: Run** `ctest --test-dir build -R 'test_booking_rules|test_api_booking|test_api_admin' --output-on-failure` — Expected: compile error (signature) / FAIL.
- [ ] **Step 3: Implement**; update all `br_apply_trip` callers to pass `&old`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Remove** the backlog line part "return sets `km_pending` even if the admin entered km during the trip" from `docs/backlog.md`.
- [ ] **Step 6: Commit** `fix(server): return keeps manual km; trip time edit reopens telematics km`

---

### Task 5: Tracker ingestion

**Files:**
- Create: `server/src/http/tracker.{h,c}`, `server/test/test_tracker.c`
- Modify: `server/src/app.h` (`int tracker_port;` in `config_t`), `server/src/config.c`, `server/src/http/server.c`, `server/CMakeLists.txt`, `server/fuzz/fuzz_teltonika.c`
- Test: `server/test/test_tracker.c`, `server/test/test_config.c`

**Interfaces:**
- Consumes: Task 2 `db_car_by_imei`, `db_add_readings`; Task 3 parser, `th_tlt_packet`.
- Produces (`http/tracker.h`):
  ```c
  typedef struct { int64_t car_id; int64_t last_rx; } tracker_conn_t;  /* car_id 0: before handshake */
  /* Handles at most one message from buf. *used = bytes consumed (0: need more).
   * reply (cap >= 4) gets the bytes to send, *reply_len may be 0.
   * Returns 0 keep open, -1 close after sending reply. */
  int tracker_feed(app_t *a, tracker_conn_t *st, const uint8_t *buf, size_t len, int64_t now,
                   uint8_t *reply, size_t *reply_len, size_t *used);
  ```
  Behaviour: before handshake parse IMEI → known (`db_car_by_imei`, active or not) reply `0x01`, set `car_id`; unknown or invalid reply `0x00`, return -1. After handshake: a lone `0xFF` byte (keep-alive) is consumed without reply; otherwise parse a packet. Invalid → -1, no reply. Complete → readings for records with `has_odometer` and `ts = ts_ms / 1000` in `[now - 90*86400, now + 600]`, skipping any whose `(car_id, ts)` already exists in `db->readings`, `received_at = now`; `db_add_readings` (skip the call if none); failure → -1 without reply; success → reply N1 as 4-byte big-endian (all records, also skipped ones).
- `config.c`: key `tracker_port`, decimal `1..65535`, else load fails; absent → 0.
- `server.c`: if `cfg.tracker_port > 0`, `mg_listen(&mgr, "tcp://0.0.0.0:<port>", tracker_ev, a)`; failure → log and return -1 like the HTTP listener. `tracker_ev`: keep `tracker_conn_t` in `c->data` (`_Static_assert(sizeof(tracker_conn_t) <= MG_DATA_SIZE)`); `MG_EV_ACCEPT`: more than 16 open tracker connections → close; `MG_EV_READ`: loop `tracker_feed` over `c->recv`, `mg_send` the reply, `mg_iobuf_del` the used bytes, close on -1 (`c->is_draining = 1`), close if `c->recv.len > TLT_MAX_DATA_LEN + 12`; `MG_EV_POLL`: close if `now - last_rx > 900`. Never log record contents.

- [ ] **Step 1: Write the failing tests** in `test_tracker.c` (car 1 with IMEI `356307042441013`, `now = T`):
  - `test_handshake_known_imei`: reply `01`, keep open, `car_id` set.
  - `test_handshake_inactive_car_accepted`.
  - `test_unknown_imei_rejected`: reply `00`, returns -1, no readings.
  - `test_packet_stores_readings_and_acks`: 3 records (one without odometer) → 2 readings, reply `00 00 00 03`.
  - `test_packet_split_across_calls`: the packet fed in two halves → first call `used == 0`, second stores.
  - `test_resend_does_not_duplicate`: same packet twice → 2 readings, both ACKed with `00 00 00 03`.
  - `test_bad_crc_closes_without_ack`: -1, `reply_len == 0`, no readings.
  - `test_oversize_length_closes`: header with data length `TLT_MAX_DATA_LEN + 1` → -1.
  - `test_packet_before_handshake_rejected`: packet bytes as first message → -1 (fails IMEI parse).
  - `test_keepalive_ff`: after handshake `FF` → `used 1`, no reply, open.
  - `test_implausible_timestamp_skipped`: record at `now + 3600` s → not stored, still counted in ACK.
  - `test_storage_failure_no_ack`: data dir made read-only (`chmod 0500`) → -1, `reply_len 0`.
  
  `test_config.c`: `test_tracker_port_parsed` (5027), `test_tracker_port_absent_is_zero`, `test_tracker_port_invalid` (`0`, `70000`, `abc` → -1).
- [ ] **Step 2: Run** `ctest --test-dir build -R 'test_tracker|test_config' --output-on-failure` — Expected: compile errors.
- [ ] **Step 3: Implement** `tracker.c`, config key, listener in `server.c`.
- [ ] **Step 4: Extend `fuzz_teltonika.c`**: also feed the input through `tracker_feed` on a `th_app()` with one tracker car (prefix the IMEI message so the packet path is reached).
- [ ] **Step 5: Run** `ctest --test-dir build --output-on-failure` — all pass. Manual smoke: start `ocs-server` with `tracker_port = 5027` and a test car, send the IMEI and a `th_tlt_packet`-style packet with `printf '...' | nc localhost 5027 | xxd` → `01` then `00000001`.
- [ ] **Step 6: Commit** `feat(server): Teltonika tracker TCP ingestion`

---

### Task 6: km jobs and admin mail

**Files:**
- Create: `server/src/http/api_km.c`, `server/test/test_api_km.c`
- Modify: `server/src/http/api.h`, `server/src/http/server.c`, `server/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 `db_put_km_mail_day`, `db_prune_readings`; Task 3 `km_resolve`.
- Produces (`http/api.h`):
  ```c
  /* Resolves km_pending DONE bookings of cars with tracker_imei (spec §8). Sets km, km_source
   * TELEMATICS, km_pending false, modified_at now, modified_by 0. Returns number resolved, -1 storage error. */
  int api_resolve_km(app_t *a, int64_t now);
  /* Daily mail to all active admins listing km_pending bookings of tracker cars with
   * now - actual_end > 86400. 1 sent, 0 nothing to send or already sent this UTC day, -1 all sends failed. */
  int api_km_pending_mail(app_t *a, int64_t now);
  ```
  Mail: subject `km ausstehend`; body `Für folgende Fahrten fehlen seit über 24 Stunden die km vom Tracker:\n\n` + one line per booking `Buchung <id>: <car name> (<plate>), <user name>\n` + `\nBitte Tracker prüfen oder km manuell eintragen:\n<base_url>/#/admin/buchungen\n`. After at least one successful send, `db_put_km_mail_day(now / 86400)`.
- `server.c`: `KM_INTERVAL_MS (300ULL * 1000ULL)` timer (`MG_TIMER_REPEAT | MG_TIMER_RUN_NOW`) calls `api_resolve_km` then `api_km_pending_mail`, logging failures like `expire_cb`. `prune_cb` also calls `db_prune_readings(&a->db, now - 90 * 86400)`.

- [ ] **Step 1: Write the failing tests** in `test_api_km.c` (tracker car 1, car 2 without tracker, admin + inactive admin):
  - `test_resolve_sets_telematics_km`: returned booking + readings → 1 resolved, km 1000/1012, `km_source KM_TELEMATICS`, not pending, persisted after reload.
  - `test_resolve_waits_without_later_reading` → 0 resolved, still pending.
  - `test_resolve_skips_car_without_tracker` → still pending.
  - `test_resolve_skips_manual` → booking with `km_pending` false untouched.
  - `test_mail_after_24h_to_active_admins_only`: pending booking with `actual_end = now - 86401` → returns 1, `th_mail_count() == 1` (active admin only), body contains `Buchung <id>` and `/#/admin/buchungen`.
  - `test_mail_not_before_24h`: `actual_end = now - 3600` → 0, no mail.
  - `test_mail_once_per_day_survives_restart`: send, `db_free` + `db_load`, call again same day → 0; next day (`now + 86400`) → 1.
  - `test_mail_ignores_car_without_tracker`.
  - `test_mail_all_fail_retries`: `th_mail_fail(1)` → -1, `km_mail_day` unchanged; then `th_mail_fail(0)` same day → 1.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_km --output-on-failure` — Expected: compile errors.
- [ ] **Step 3: Implement** `api_km.c` and the timers (copy-before-modify around `db_put_booking`, like `api_expire_bookings`).
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Commit** `feat(server): resolve km from tracker readings, daily pending mail`

---

### Task 7: Admin car IMEI and km display

**Files:**
- Modify: `server/src/http/api_admin.c` (`read_car_fields`), `web/js/views/admin_cars.js`, `web/js/views/admin_bookings.js`, `web/sw.js`
- Test: `server/test/test_api_admin.c`

**Interfaces:**
- Consumes: Task 1 `imei_valid`, Task 2 `db_car_by_imei`.
- `read_car_fields`: optional `"tracker_imei"` string; absent keeps the current value, `""` clears, otherwise `imei_valid` or 400 `invalid`. Another car already holding the IMEI → 409 `duplicate_imei`, message `Diese IMEI ist bereits einem Auto zugeordnet.`

- [ ] **Step 1: Write the failing tests** in `test_api_admin.c`: `test_car_post_with_imei` (201, echoed), `test_car_put_clears_imei` (`""`), `test_car_put_without_imei_keeps_it`, `test_car_bad_imei_400`, `test_car_duplicate_imei_409`, `test_car_same_imei_on_own_car_ok` (PUT of the car that already has it → 200).
- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_admin --output-on-failure` — Expected: FAIL.
- [ ] **Step 3: Implement** the server part.
- [ ] **Step 4: Web.** `admin_cars.js`: text field `Tracker-IMEI` (optional, `inputmode="numeric"`, `pattern="\d{15}"`), sent as `tracker_imei` (trimmed); the list line shows `Tracker <imei>` or `ohne Tracker`; the "Deaktivieren" body is unchanged (absent keeps the IMEI). `admin_bookings.js`: km line suffix ` · Tracker` for `km_source === 'telematics'` next to the existing ` · manuell`. Bump `CACHE` in `web/sw.js`.
- [ ] **Step 5: Run** `ctest --test-dir build --output-on-failure && (cd web && node --test test/)` — all pass. Manually: create a car with IMEI in the admin UI, edit, clear it.
- [ ] **Step 6: Commit** `feat: tracker IMEI in car admin`

---

### Task 8: Deployment docs

**Files:**
- Modify: `deploy/vps/ocs.conf.example`, `deploy/vps/README.md`

- [ ] **Step 1: `ocs.conf.example`**: commented block `tracker_port = 5027` explaining it is the Teltonika TCP port, open to the internet, absent = disabled.
- [ ] **Step 2: `README.md`**: add `sudo ufw allow 5027/tcp` to the firewall step (only if `tracker_port` is set); new section "Teltonika tracker" with the Configurator settings, each marked "check names in your firmware's Configurator":
  - GPRS: APN of the SIM (1NCE: `iot.1nce.net`), server domain = the VPS host name, port `5027`, protocol TCP.
  - System/Data protocol: Codec 8 Extended.
  - Data acquisition: on moving every 300 s, on stop every 3600 s; records on movement start/stop; movement source movement sensor + GNSS (not ignition).
  - I/O: Total Odometer enabled, odometer source GNSS, initial value = dashboard km × 1000 (metres).
  - Add the IMEI (device label) in the admin car form.
  - Check: after the first drive the car's readings appear (`tail data/odometer_readings.jsonl`).
- [ ] **Step 3: Commit** `docs(deploy): Teltonika tracker port and configuration`

---

### Final verification

- [ ] `cmake --build build && ctest --test-dir build --output-on-failure`, `(cd web && node --test test/)`, 60 s runs of `fuzz_api` and `fuzz_teltonika`. Expected: all green, no fuzz crash. Then superpowers:finishing-a-development-branch; after the merge mark Phase 3 done in `docs/spec.md` §15.
