# Phase 2: Trip Lifecycle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Users start and return bookings, report damage with photos, and see past bookings with actual times and km; admins fix trip times and km and view damage reports.

**Architecture:** Trip rules are pure functions in `core/booking_rules.c` next to the Phase 1 rules; the API layer (`http/api_booking.c`, new `http/api_damage.c`, `http/api_admin.c`) only parses, calls a rule, and persists. Damage reports are a new JSONL table; photos are raw JPEG request bodies stored as files under `data/photos/`. `api_res_t` gains a binary body so photos are served through the same `api_handle` path that tests call directly.

**Tech Stack:** unchanged from Phase 1: C11, CMake, Mongoose 7.14, cJSON, Unity, vanilla JS (ES2020), `node --test`.

**Spec:** `docs/spec.md` (§4 booking/damage_report, §5, §13, §15 Phase 2) and `docs/backlog.md` (items tagged `[P2]`).

**Branch:** `phase2-trip-lifecycle`.

**Product-owner decisions (2026-10-31):**
- Photos are uploaded as raw `image/jpeg` bodies, one per request (replaces "multipart" in spec §13).
- Admin fixes trips by editing `actual_start`/`actual_end`/km in the admin booking PUT; the state follows from the actual times.
- Early start is allowed at any time before `booked_end` if the car is free from now until `booked_start`.
- "Keine Schäden" stores nothing; only real damage creates a report.

**Out of scope:** Geotab (Phase 3), prices (Phase 4), PIN (§7, later), booking mails (Phase 7).

## Global Constraints

- All Phase 1 Global Constraints in `docs/superpowers/plans/2026-10-30-phase1-booking-core.md` still hold (time in `int64_t` UTC seconds, no float, German UI, English code, fixed `char[]` with reject-not-truncate, `-Wall -Wextra -Werror`, ASan/UBSan tests).
- New data files: `damage_reports.jsonl`, directory `photos/` (mode 0700). Data written by Phase 1 must still load: every new booking key is optional on load.
- Photo: `Content-Type: image/jpeg`, body ≤ 2 MiB (`2097152`), must start with bytes `FF D8 FF`; at most 10 photos per report.
- Damage text: required, non-empty, `strlen < 1024`.
- km values: integers in `[0, 10000000]`, `km_end >= km_start`.
- Overdue grace: an `active` booking with `now >= booked_end` occupies its car until `now + 1800` s.
- Actual times are stored to the second (no 60 s alignment); admin-entered actual times must be `<= now`.
- db contract (backlog): copy before modify, never pass a pointer into the db to `db_put_*`.
- Every changed web file is listed in `web/sw.js` `SHELL`; bump `CACHE` once per task that changes web files.

## Review Focus

1. **Previous driver overdue:** user B tries to start while user A's booking on the same car is still `active` past its end → 409 `car_busy`; B's calendar shows A's booking as occupied through now + 30 min. Tests: Task 2 `test_start_blocked_by_overdue_active`, Task 3 `test_calendar_shows_overdue_active`.
2. **Early start then someone books the gap:** A starts at 08:00 for a 10:00 booking; B tries to book 08:30–09:30 → 409 `overlap`. Test: Task 2 `test_early_start_occupies_from_actual_start`.
3. **Bad uploads leave no trace:** a PNG, a 3 MB file, or an 11th photo is rejected (415 / 413 / 400 `too_many_photos`) and `n_photos` and `data/photos/` are unchanged. Tests: Task 5 `test_photo_rejects_non_jpeg`, `test_photo_too_large_413`, `test_photo_limit_10`.
4. **Phase 1 data after upgrade:** a `bookings.jsonl` line without any trip key loads with `state` kept, `actual_*` 0, `km_source` `none`, `km_pending` false. Test: Task 1 `test_booking_loads_phase1_line`.
5. **Damage photos are private:** another user requesting a report or photo of someone else's booking gets 404, admin gets 200. Test: Task 5 `test_damage_visible_to_owner_and_admin_only`.

---

## File Structure

```
server/src/core/model.h            + km_source_t, trip fields in booking_t, damage_report_t
server/src/core/codec.{h,c}        + trip keys (optional on load), damage_report codec
server/src/store/store.{h,c}       + store_read_file, store_ensure_dir
server/src/store/db.{h,c}          + damage_reports table, photo path, photos/ dir on load
server/src/core/booking_rules.{h,c} occupancy with actual_start and grace; start/return/trip/damage rules
server/src/http/api.{h,c}          binary request/response, per-route body kind and limit, new routes, expire sweep
server/src/http/api_internal.h     new handler declarations
server/src/http/api_booking.c      start, return, scope=history
server/src/http/api_damage.c       NEW: damage reports and photos
server/src/http/api_admin.c        trip fields and km in admin PUT
server/src/http/server.c           per-route body limit, binary reply, expire timer
server/test/test_api_damage.c      NEW
server/fuzz/fuzz_api.c             image/jpeg bodies for JPEG routes
web/js/trip.js                     NEW: pure tripActions()
web/js/photo.js                    NEW: fitSize(), resizeToJpeg()
web/js/api.js                      + apiUpload()
web/js/views/damage_dialog.js      NEW: "Keine Schäden" / report dialog
web/js/views/my_bookings.js        Starten / Zurückgeben, no Stornieren when started
web/js/views/history.js            NEW: #/verlauf
web/js/views/admin_bookings.js     actual times, km, damage view
web/test/trip.test.mjs             NEW
```

---

### Task 1: Model, codec and store for trip data and damage reports

**Files:**
- Modify: `server/src/core/model.h`, `server/src/core/codec.{h,c}`, `server/src/store/store.{h,c}`, `server/src/store/db.{h,c}`
- Test: `server/test/test_codec.c`, `server/test/test_db.c`

**Interfaces:**
- Produces:
  - `typedef enum { KM_NONE = 0, KM_GEOTAB, KM_MANUAL } km_source_t;` JSON names `"none"`, `"geotab"`, `"manual"`.
  - `booking_t` gains `int64_t actual_start, actual_end, km_start, km_end; km_source_t km_source; bool km_pending;` (0 = unset for `actual_*`).
  - `typedef enum { DMG_START = 0, DMG_END = 1 } damage_phase_t;` JSON `"start"`, `"end"`.
  - `#define DMG_MAX_PHOTOS 10`
  - `typedef struct { int64_t id, booking_id, user_id, created_at; damage_phase_t phase; char text[1024]; int n_photos; } damage_report_t;`
  - `cJSON *damage_report_to_json(const damage_report_t *v); int damage_report_from_json(const cJSON *j, damage_report_t *out);` (all keys required)
  - `int store_read_file(const char *path, size_t max, char **out, size_t *len);` 0 ok (caller frees `*out`), -1 missing, unreadable or larger than `max`.
  - `int store_ensure_dir(const char *path);` `mkdir 0700`, `EEXIST` is success.
  - `db_t` gains `damage_report_t *damages; size_t n_damages, cap_damages;`
  - `damage_report_t *db_damage(db_t *db, int64_t id); int db_put_damage(db_t *db, damage_report_t *d);` (same id/rollback contract as `db_put_booking`)
  - `int db_photo_path(const db_t *db, int64_t report_id, int k, char *out, size_t cap);` → `<dir>/photos/r<report_id>-<k>.jpg`, k is 1-based; -1 if it does not fit.
  - `db_load` creates `<dir>/photos` and loads `damage_reports.jsonl`.

- [ ] **Step 1: Write failing tests**

```c
/* test_codec.c */
void test_booking_trip_roundtrip(void) {
  booking_t b = {.id = 1, .car_id = 2, .user_id = 3, .booked_start = 600, .booked_end = 1200,
                 .state = BK_DONE, .actual_start = 590, .actual_end = 1300, .km_start = 1000,
                 .km_end = 1042, .km_source = KM_MANUAL, .km_pending = false};
  cJSON *j = booking_to_json(&b);
  TEST_ASSERT_EQUAL_STRING("manual", cJSON_GetObjectItem(j, "km_source")->valuestring);
  booking_t r; TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
  TEST_ASSERT_EQUAL_INT64(590, r.actual_start); TEST_ASSERT_EQUAL_INT64(1300, r.actual_end);
  TEST_ASSERT_EQUAL_INT64(1042, r.km_end); TEST_ASSERT_EQUAL_INT(KM_MANUAL, r.km_source);
  cJSON_Delete(j);
}
void test_booking_loads_phase1_line(void) {
  cJSON *j = cJSON_Parse("{\"id\":1,\"car_id\":1,\"user_id\":1,\"booked_start\":600,\"booked_end\":1200,"
                         "\"state\":\"active\",\"cancelled_at\":0,\"cancel_fee_cents\":0,\"created_at\":1,"
                         "\"modified_at\":1,\"modified_by\":1}");
  booking_t r; TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
  TEST_ASSERT_EQUAL_INT(BK_ACTIVE, r.state); TEST_ASSERT_EQUAL_INT64(0, r.actual_start);
  TEST_ASSERT_EQUAL_INT(KM_NONE, r.km_source); TEST_ASSERT_FALSE(r.km_pending);
  cJSON_Delete(j);
}
void test_booking_rejects_bad_km_source(void);   /* "km_source":"gps" -> -1 */
void test_damage_roundtrip(void);                /* all fields survive; phase "end"; text with umlauts */
void test_damage_rejects_long_text(void);        /* 1024 chars -> -1 */

/* test_db.c */
void test_damage_put_and_reload(void);           /* put assigns id 1, survives db_free + db_load */
void test_load_creates_photos_dir(void);         /* stat(<dir>/photos) is a dir after db_load */
void test_photo_path(void);                      /* db_photo_path(db, 7, 2, ...) ends with "/photos/r7-2.jpg" */
void test_store_read_file_limit(void);           /* 10-byte file: max 10 -> 0, len 10; max 9 -> -1; missing -> -1 */
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ctest --test-dir build -R 'test_codec|test_db' --output-on-failure`
Expected: compile errors for the new fields and functions.

- [ ] **Step 3: Implement the Interfaces above.** In `codec.c` add `get_i64_opt` and `get_enum_opt` (absent key → 0 / first value) beside `get_bool_opt` and use them for the six trip keys; write all six keys always. Add `DEFINE_LOADER_X(damages, ...)` and a `put_at` wrapper like the bookings one.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir build --output-on-failure`
Expected: all tests pass, including the Phase 1 ones.

- [ ] **Step 5: Commit** — `feat(server): booking trip fields and damage report table`

---

### Task 2: Trip rules in `core/booking_rules`

**Files:**
- Modify: `server/src/core/booking_rules.{h,c}`
- Test: `server/test/test_booking_rules.c`

**Interfaces:**
- Consumes: Task 1 `booking_t` trip fields, `damage_phase_t`.
- Produces:
  - `#define BR_OVERDUE_GRACE 1800`
  - `br_result_t` gains `BR_CAR_BUSY`, `BR_EXPIRED`.
  - `int64_t booking_occupied_start(const booking_t *b);` `ACTIVE` with `actual_start > 0`: `min(booked_start, actual_start)`; else `booked_start`.
  - `int64_t booking_occupied_end(const booking_t *b, int64_t now);` changed: `ACTIVE` and `now >= booked_end` → `now + BR_OVERDUE_GRACE`; else `booked_end`.
  - `br_check_free` compares against `[booking_occupied_start(b), booking_occupied_end(b, now))`.
  - `br_result_t br_check_start(const db_t *db, const user_t *actor, const booking_t *b, int64_t now);`
    Order: `BR_FORBIDDEN` (not own) → `BR_NOT_MODIFIABLE` (state ≠ `RESERVED`) → `BR_EXPIRED` (`now >= booked_end`) → `BR_CAR_INACTIVE` → `BR_CAR_BUSY` (`br_check_free` on `[min(now, booked_start), booked_end)` excluding `b->id` fails).
    `start_tolerance_min` needs no own check: inside the tolerance window the rule above already decides (a not-yet-returned previous booking makes the car busy).
  - `br_result_t br_check_return(const user_t *actor, const booking_t *b);` `BR_FORBIDDEN` → `BR_NOT_MODIFIABLE` (state ≠ `ACTIVE`).
  - `br_result_t br_apply_trip(booking_t *nb, int64_t now);` admin only, called when the request carried `actual_start` or `actual_end`. Valid: both 0 → `RESERVED`, `km_pending=false`; start only → `ACTIVE`, `km_pending=false`; both, `end > start` → `DONE`, `km_pending = (km_source == KM_NONE)`. `BR_INVALID` for end without start, `end <= start`, or a time not in `[0, now]`.
  - `br_result_t br_check_damage(const user_t *actor, const booking_t *b, damage_phase_t phase);` `BR_FORBIDDEN` (not own) → `BR_NOT_MODIFIABLE` unless (`DMG_START` and `actual_start > 0` and state `ACTIVE` or `DONE`) or (`DMG_END` and state `DONE` and `actual_end > 0`).
  - `br_check_update`: free-check gate uses `nb->state` (backlog `[P2]`); user club-flag change is `BR_NOT_MODIFIABLE` when `now >= old->booked_start || old->state != BK_RESERVED` (backlog `[P2]`).

- [ ] **Step 1: Write failing tests** (existing helpers `add()`, `mk()`, `now = T`, `H = 3600`)

```c
void test_overdue_active_blocks_until_now_plus_grace(void) {
  booking_t *b = add(1, 1, 1, T - 2 * H, T - H, BK_ACTIVE);
  TEST_ASSERT_EQUAL_INT64(T + BR_OVERDUE_GRACE, booking_occupied_end(b, T));
  TEST_ASSERT_EQUAL_INT64(T - H, booking_occupied_end(b, T - H - 60));
}
void test_early_start_occupies_from_actual_start(void) {
  booking_t *b = add(1, 1, 1, T + 2 * H, T + 4 * H, BK_ACTIVE); b->actual_start = T;
  TEST_ASSERT_EQUAL_INT(BR_OVERLAP, br_check_free(&db, 1, T + 1800, T + 5400, 0, T));
}
void test_start_in_window_ok(void);               /* RESERVED [T+10min, T+2H), own, car free -> BR_OK */
void test_early_start_free_ok(void);              /* [T+5H, T+6H), nothing before -> BR_OK */
void test_early_start_blocked_by_reserved(void);  /* other RESERVED [T+H, T+2H), own [T+3H, T+4H) -> BR_CAR_BUSY */
void test_start_blocked_by_overdue_active(void) {
  booking_t *a1 = add(1, 1, 2, T - 2 * H, T - 600, BK_ACTIVE); a1->actual_start = T - 2 * H;
  booking_t *own = add(2, 1, 1, T - 600, T + H, BK_RESERVED);
  TEST_ASSERT_EQUAL_INT(BR_CAR_BUSY, br_check_start(&db, &users[0], own, T));
}
void test_start_after_booked_end_expired(void);   /* [T-2H, T-H) -> BR_EXPIRED */
void test_start_twice_not_modifiable(void);       /* ACTIVE -> BR_NOT_MODIFIABLE */
void test_start_other_user_forbidden(void);
void test_start_inactive_car(void);
void test_return_rules(void);                     /* own ACTIVE -> OK; RESERVED/DONE -> NOT_MODIFIABLE; other -> FORBIDDEN */
void test_apply_trip_derives_state(void);         /* (0,0)->RESERVED; (s,0)->ACTIVE; (s,e)->DONE + km_pending; with KM_MANUAL -> km_pending false */
void test_apply_trip_invalid(void);               /* (0,e); e<=s; s>now -> BR_INVALID */
void test_damage_phase_rules(void);               /* start ok on ACTIVE/DONE with actual_start; end only DONE; RESERVED -> NOT_MODIFIABLE */
void test_club_flag_fixed_after_early_start(void);/* ACTIVE, actual_start T-H, booked_start T+H: user flag change -> NOT_MODIFIABLE */
void test_admin_reopen_done_checks_overlap(void); /* admin nb DONE->ACTIVE overlapping another RESERVED -> BR_OVERLAP (gate on nb->state) */
/* backlog [P2] test gaps */
void test_active_extension_overlap(void);         /* user extends ACTIVE end into next RESERVED -> BR_OVERLAP */
void test_user_change_car_or_user_rejected(void); /* RESERVED and ACTIVE -> BR_NOT_EXTENSION */
void test_unaligned_update_invalid(void);         /* end % 60 != 0 -> BR_INVALID */
void test_admin_update_done_ok(void);             /* admin changes booked times of DONE -> BR_OK */
```

Update `test_active_late_booking_blocks_until_now` to expect `now + BR_OVERDUE_GRACE`.

- [ ] **Step 2: Run** `ctest --test-dir build -R test_booking_rules --output-on-failure` — Expected: compile errors / FAIL.
- [ ] **Step 3: Implement the Interfaces above** in `booking_rules.{h,c}`; extend the check-order comment at the top of `booking_rules.c` with the start order.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — Expected: all pass.
- [ ] **Step 5: Commit** — `feat(server): trip rules for start, return, admin trip edit, damage`; remove the four `[P2]` rule items (free-check gate, club flag after early start, test gaps) from `docs/backlog.md` in the same commit.

---

### Task 3: Start, return, history and expiry API

**Files:**
- Modify: `server/src/http/api_booking.c`, `server/src/http/api.{h,c}`, `server/src/http/api_internal.h`, `server/src/http/server.c`
- Test: `server/test/test_api_booking.c`

**Interfaces:**
- Consumes: Task 2 `br_check_start`, `br_check_return`, `booking_occupied_*`.
- Produces:
  - `POST /api/bookings/{id}/start` (own booking, empty body) → 200 booking JSON; `state "active"`, `actual_start = now`.
  - `POST /api/bookings/{id}/return` → 200; `state "done"`, `actual_end = now`, `km_pending true`.
  - `GET /api/bookings?scope=upcoming|history`; absent = `upcoming` (Phase 1 behavior); other value → 400 `invalid`. `history`: own bookings with state `done` or `cancelled`, sorted by `booked_start` descending.
  - Calendar `end` uses `booking_occupied_end`, `start` uses `booking_occupied_start`.
  - `api_br_error`: `BR_CAR_BUSY` → 409 `car_busy` "Das Auto ist zu diesem Zeitpunkt nicht frei."; `BR_EXPIRED` → 400 `expired` "Die Buchungszeit ist bereits abgelaufen."
  - `int api_expire_bookings(app_t *a, int64_t now);` in `api.h`: every `RESERVED` booking with `booked_end <= now` becomes `DONE` (no actual times, `km_pending false`, `modified_at = now`, `modified_by = 0`). Returns count, -1 on storage error. `server.c` runs it on an `MG_TIMER_REPEAT | MG_TIMER_RUN_NOW` timer every 300000 ms.
  - Handlers `h_booking_start`, `h_booking_return` declared in `api_internal.h`, routes added to `ROUTES`.

- [ ] **Step 1: Write failing tests**

```c
void test_start_and_return_flow(void) {
  /* book [T+10min, T+2H) as u1, start at T, return at T+H */
  /* start -> 200, state "active", actual_start == T */
  /* return at T+H -> 200, state "done", actual_end == T+H, km_pending true */
}
void test_start_twice_400(void);                 /* second start -> 400 not_modifiable */
void test_start_other_users_booking_404(void);
void test_start_blocked_409_car_busy(void);      /* u2 overdue ACTIVE on car1 -> u1 start 409 "car_busy" */
void test_start_expired_400(void);
void test_cancel_after_start_rejected(void);     /* DELETE after start -> 400 not_modifiable */
void test_extend_active_ok(void);                /* PUT end later on ACTIVE -> 200 */
void test_calendar_shows_overdue_active(void) {
  /* u2 booking [T-2H, T-H) ACTIVE; GET /api/calendar?car=..&from=T&to=T+86400 as u1
     -> one entry, own false, end == T + 1800 */
}
void test_history_scope(void);                   /* done + cancelled own, desc order; RESERVED and other users' excluded */
void test_scope_invalid_400(void);               /* scope=all -> 400 invalid */
void test_expire_sweep(void);                    /* RESERVED ending T-60 -> DONE, actual_start 0, km_pending false; ACTIVE untouched; returns 1 */
```

- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_booking --output-on-failure` — Expected: FAIL (404 for new routes).
- [ ] **Step 3: Implement the Interfaces above.** History reuses `collect()` with a `scope` enum instead of the `calendar` bool; descending order via its own comparator.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — Expected: all pass.
- [ ] **Step 5: Update spec §13** line `GET /api/bookings?scope=mine|upcoming` → `GET /api/bookings?scope=upcoming|history`; remove the `[P2]` calendar item from `docs/backlog.md`.
- [ ] **Step 6: Commit** — `feat(server): start/return, booking history, expire past reservations`

---

### Task 4: Admin trip edit and manual km

**Files:**
- Modify: `server/src/http/api_admin.c`
- Test: `server/test/test_api_admin.c`

**Interfaces:**
- Consumes: Task 2 `br_apply_trip`, `br_check_update` with `nb->state` gate.
- Produces: `PUT /api/admin/bookings/{id}` accepts optional keys
  - `actual_start`, `actual_end`: integer or `null` (= 0). If either key is present, `br_apply_trip(&nb, now)` runs after reading and before `br_check_update`; otherwise state is unchanged.
  - `km_start`, `km_end`: both or neither, integers per Global Constraints → `km_source "manual"`, `km_pending false`. One without the other, out of range, or `km_end < km_start` → 400 `invalid`.
  - Applies to `reserved`, `active`, `done`; `cancelled` stays 400 `not_modifiable`.
  - Response as before (`booking_admin_json`), now carrying the trip keys.

- [ ] **Step 1: Write failing tests**

```c
void test_admin_close_forgotten_trip(void);      /* ACTIVE, PUT actual_end=T -> state "done", km_pending true */
void test_admin_mark_started(void);              /* RESERVED, PUT actual_start=T-H -> "active" */
void test_admin_clear_actuals_reserved(void);    /* DONE, actual_start=null, actual_end=null -> "reserved" */
void test_admin_manual_km(void) {
  /* DONE with km_pending: PUT km_start 1000, km_end 1042 -> km_source "manual", km_pending false, km_end 1042 */
}
void test_admin_km_invalid(void);                /* only km_start; km_end 999 < 1000; km 10000001 -> 400 */
void test_admin_actual_in_future_invalid(void);  /* actual_start T+60 -> 400 */
void test_admin_trip_on_cancelled_400(void);
void test_admin_put_without_actuals_keeps_state(void); /* expired DONE (no actuals), PUT booked times only -> still "done" */
```

- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_admin --output-on-failure` — Expected: FAIL.
- [ ] **Step 3: Extend `read_booking_fields`** (or a sibling `read_trip_fields(const cJSON*, booking_t*, bool *has_actual)`) and `h_admin_booking_put` per the Interfaces.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — Expected: all pass.
- [ ] **Step 5: Commit** — `feat(server): admin edits actual times and manual km`

---

### Task 5: Damage reports and photos API

**Files:**
- Create: `server/src/http/api_damage.c`, `server/test/test_api_damage.c`
- Modify: `server/src/http/api.{h,c}`, `server/src/http/api_internal.h`, `server/src/http/server.c`, `server/CMakeLists.txt` (`ocs_add_test(test_api_damage)`), `server/fuzz/fuzz_api.c`, `server/test/helpers.{h,c}`
- Test: `server/test/test_api_damage.c`

**Interfaces:**
- Consumes: Task 1 damage table and `db_photo_path`, `store_read_file`, `store_write_atomic`; Task 2 `br_check_damage`.
- Produces:
  - `api_res_t` gains `char content_type[32]` (empty = `application/json`) and `size_t body_len` (0 = `strlen(body)`).
  - Route row gains `body_kind_t body_kind` (`BODY_JSON` default, `BODY_JPEG`). For `BODY_JPEG` routes the content type must be `image/jpeg` (else 415), `ctx->body` is NULL, the handler reads `req->body`/`req->body_len`.
  - `size_t api_body_limit(const char *method, const char *path);` → `2097152` for `BODY_JPEG` routes, `65536` otherwise. `server.c` uses it in place of `MAX_BODY` for both the HDRS and MSG checks; `api_handle` also answers 413 `too_large` when `body_len` exceeds it (so tests and the fuzzer see the limit).
  - `server.c` replies with `res.content_type` and `res.body_len` via `mg_printf` + `mg_send` when `content_type` is set.
  - Routes (all `MIN_USER`):
    - `POST /api/bookings/*/damage` JSON `{"phase":"start"|"end","text":"..."}` → 201 report JSON. Own booking only.
    - `GET /api/bookings/*/damage` → 200 array of reports; booking owner or admin, else 404.
    - `POST /api/damage/*/photos` (`BODY_JPEG`) → 201 report JSON with `n_photos` incremented. Owner of the report only. Writes the file first, then `db_put_damage`; on db failure unlinks the file and returns 500.
    - `GET /api/damage/*/photos/*` → 200 `image/jpeg`; owner or admin; k outside `1..n_photos` → 404.
  - Errors: > 10 photos → 400 `too_many_photos` "Höchstens 10 Fotos pro Schadensmeldung."; not starting with `FF D8 FF` → 415 `unsupported_media`; > 2 MiB → 413 `too_large`.
  - `helpers.h`: `api_res_t th_req_bin(app_t *a, const char *method, const char *path, const void *body, size_t len, const char *token, int64_t now, const char *content_type);`

- [ ] **Step 1: Write failing tests** (setUp: u1 owns a started booking, u2 other user, admin)

```c
static const unsigned char JPG[] = {0xFF, 0xD8, 0xFF, 0xE0, 'J', 'F', 'I', 'F', 0, 0xFF, 0xD9};

void test_report_create_and_list(void);          /* POST phase start text "Kratzer vorne links" -> 201; GET -> 1 entry */
void test_report_text_required(void);            /* "" and 1024 chars -> 400 invalid */
void test_report_end_needs_return(void);         /* phase end on ACTIVE -> 400 not_modifiable */
void test_photo_upload_and_get(void) {
  /* POST JPG -> 201, n_photos 1; GET /api/damage/<rid>/photos/1 -> 200, content_type "image/jpeg",
     body_len == sizeof JPG, memcmp equal */
}
void test_photo_rejects_non_jpeg(void);          /* PNG magic -> 415; content type application/json -> 415; n_photos 0, no file */
void test_photo_too_large_413(void);             /* api_body_limit("POST","/api/damage/1/photos") == 2097152;
                                                    handler given 2097153 bytes -> 413 */
void test_photo_limit_10(void);                  /* 10 uploads ok, 11th -> 400 too_many_photos, no r<id>-11.jpg */
void test_damage_visible_to_owner_and_admin_only(void); /* u2 GET report list and photo -> 404; admin -> 200 */
void test_photo_upload_by_admin_404(void);       /* admin is not the owner -> 404 */
void test_photo_storage_failure_unlinks(void);   /* replace <dir>/damage_reports.jsonl with a non-empty directory so the
                                                    rename fails -> 500, n_photos unchanged, no r<id>-1.jpg */
void test_photo_index_out_of_range_404(void);    /* k=0, k=2 with one photo, k="x" */
```

- [ ] **Step 2: Run** `cmake --build build && ctest --test-dir build -R test_api_damage --output-on-failure` — Expected: FAIL.
- [ ] **Step 3: Implement the Interfaces above.** Fuzz harness: for `BODY_JPEG` routes send the fuzz input with `image/jpeg`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — Expected: all pass. Then build the server and check by hand:
  `curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: image/jpeg' --data-binary @big3mb.jpg http://127.0.0.1:<port>/api/damage/1/photos` → `413` (also unauthenticated: the size check runs at HDRS).
- [ ] **Step 5: Update spec** §4 damage_report (add `user_id`; drop "photo files (0..n)" for `n_photos`, max 10, ≤ 2 MiB; "Keine Schäden" stores nothing) and §13 (replace the `damage (multipart photo)` line with the four damage routes).
- [ ] **Step 6: Commit** — `feat(server): damage reports with JPEG photos`

---

### Task 6: Web: start/return flow, damage dialog, history

**Files:**
- Create: `web/js/trip.js`, `web/js/photo.js`, `web/js/views/damage_dialog.js`, `web/js/views/history.js`, `web/test/trip.test.mjs`
- Modify: `web/js/api.js`, `web/js/app.js` (route `'#/verlauf': 'history'`, menu entry "Verlauf"), `web/js/views/my_bookings.js`, `web/style.css`, `web/sw.js`

**Interfaces:**
- Consumes: Task 3 and Task 5 routes.
- Produces:
  - `tripActions(b, nowSec) -> { start, ret, extend, cancel }` (booleans): `start` = reserved && now < booked_end; `ret` = active; `extend` = reserved || active; `cancel` = reserved && now < booked_start.
  - `fitSize(w, h, max = 1600) -> { w, h }`: scale so the longer side ≤ max, keep aspect, round, never upscale.
  - `resizeToJpeg(file) -> Promise<Blob>`: `createImageBitmap` (with `imageOrientation: 'from-image'`) → canvas at `fitSize` → `toBlob('image/jpeg', 0.85)`.
  - `apiUpload(path, blob)`: POST with `Content-Type: image/jpeg`, same error handling as `api()`.
  - `damageDialog(title) -> Promise<null | 'none' | { text, files }>`: null = abort; buttons "Keine Schäden" / "Schaden melden" → textarea (required) + `<input type=file accept="image/*" multiple>` (max 10).

- [ ] **Step 1: Write failing tests** `web/test/trip.test.mjs`

```js
test('tripActions', () => {
  const b = { state: 'reserved', booked_start: 1000, booked_end: 2000 };
  assert.deepEqual(tripActions(b, 900), { start: true, ret: false, extend: true, cancel: true });
  assert.deepEqual(tripActions(b, 1500), { start: true, ret: false, extend: true, cancel: false });
  assert.deepEqual(tripActions(b, 2000), { start: false, ret: false, extend: true, cancel: false });
  assert.deepEqual(tripActions({ ...b, state: 'active' }, 2500), { start: false, ret: true, extend: true, cancel: false });
});
test('fitSize', () => {
  assert.deepEqual(fitSize(4000, 3000), { w: 1600, h: 1200 });
  assert.deepEqual(fitSize(3000, 4000), { w: 1200, h: 1600 });
  assert.deepEqual(fitSize(800, 600), { w: 800, h: 600 });
  assert.deepEqual(fitSize(1601, 1), { w: 1600, h: 1 });
});
```

- [ ] **Step 2: Run** `cd web && node --test` — Expected: FAIL (modules missing).
- [ ] **Step 3: Implement.** "Meine Buchungen" shows per card the buttons from `tripActions`: "Starten", "Zurückgeben", "Verlängern", "Stornieren"; an active booking shows "Unterwegs seit <time>". Start flow: `damageDialog('Fahrt starten')` → abort on null → `POST start` → if a report: `POST damage {phase:'start'}` then `apiUpload` each resized photo in order; any error shows in the page's `err` box and the list reloads (the trip state stays as the server has it). Return flow the same with `'Fahrzeug zurückgeben'` and phase `end`. "Verlauf" view: `GET /api/bookings?scope=history`, card per booking: booked range, "Gefahren: <actual_start> – <actual_end>" or "Nicht angetreten" (done without actual times) or "Storniert", km "1000 – 1042 km (42 km)" or "km ausstehend" when `km_pending`, "Vereinsfahrt" pill. Fix backlog `[any]` badge: style `.booking-car .badge` as the admin pill. Add new files to `SHELL`, bump `CACHE`.
- [ ] **Step 4: Run** `cd web && node --test && TZ=America/New_York node --test` — Expected: PASS. Then use the `run` skill: start, report damage with one phone-sized photo, return, see it in "Verlauf"; check "Stornieren" is gone on the active card.
- [ ] **Step 5: Commit** — `feat(web): start/return flow, damage reports, booking history`; remove the `[P2]` "Stornieren" item and the `[any]` badge item from `docs/backlog.md`.

---

### Task 7: Web admin: trip fields, km, damage view

**Files:**
- Modify: `web/js/views/admin_bookings.js`, `web/style.css`, `web/sw.js`

**Interfaces:**
- Consumes: Task 4 admin PUT keys; Task 5 `GET /api/bookings/{id}/damage`, photo URL `/api/damage/{rid}/photos/{k}`.

- [ ] **Step 1: Implement.** List row adds actual times, km (or "km ausstehend" badge), state label from `STATES`. "Bearbeiten" also for `done`. Edit form adds "Tatsächlicher Beginn", "Tatsächliches Ende" (date + time via `form_time.js`, each with "leeren" → sends `null`), "km Start", "km Ende" (sent only when both filled). Button "Schäden" on rows with reports: fetch list, show phase ("bei Start"/"bei Rückgabe"), `created_at`, text, photos as `<img loading="lazy">` linked to the full image. Bump `CACHE`.
- [ ] **Step 2: Verify** with the `run` skill: close a forgotten active booking by setting "Tatsächliches Ende" → row shows "beendet" and "km ausstehend"; enter km → badge gone; open "Schäden" of the Task 6 booking → text and photo visible. `cd web && node --test` still passes.
- [ ] **Step 3: Commit** — `feat(web): admin trip times, km and damage view`

---

### Task 8: Phase close

- [ ] **Step 1:** Full verification: `cmake --build build && ctest --test-dir build --output-on-failure`, `cd web && node --test`, a 60 s fuzz run of `fuzz_api` (`-DOCS_FUZZ=ON`, clang). Expected: all green, no fuzz crash.
- [ ] **Step 2:** Check "Done when" of §15 Phase 2 by hand on the running app: a booking is started, returned, reported on, and shows in "Verlauf" with actual times and km (km entered by admin).
- [ ] **Step 3:** Mark Phase 2 done in spec §15 (`— done (merged <date>)`, plan path), confirm no `[P2]` items remain in `docs/backlog.md`.
- [ ] **Step 4: Commit** — `docs: phase 2 done`
