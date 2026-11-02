# Phase 4: Tariffs and Pricing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every booking shows its price (estimate before booking, actual price afterwards), computed by one pure engine from a per-car, versioned tariff.

**Architecture:** A pure `Europe/Berlin` converter (`core/berlin.c`) and a pure pricing engine (`core/pricing.c`) carry the logic. Tariffs are a new db table. `http/api_price.c` attaches a `price` object to booking JSON on read and serves the estimate endpoint; `http/api_tariff.c` is the admin CRUD. The web form asks the server for the estimate; a pure `web/js/price.js` turns a price object into display lines.

**Tech Stack:** unchanged: C11, CMake, Mongoose 7.14, cJSON, Unity, vanilla JS (ES2020), `node --test`.

**Spec:** `docs/superpowers/specs/2026-11-02-phase4-pricing-design.md` (binding where it says so), `docs/spec.md` §3, §4 (tariff, settings), §6, §15 Phase 4.

**Branch:** `phase4-pricing` (spec commits `2fdd9dd`, design `f67711f` are on it).

**Out of scope:** invoices, SEPA, billing run, storing prices, tracker abuse protection.

## Global Constraints

- All Phase 1–3 Global Constraints still hold (`int64_t` UTC seconds, no float, German UI, English code, fixed `char[]` reject-not-truncate, `-Wall -Wextra -Werror`, ASan/UBSan tests, db copy-before-modify, every changed web file in `web/sw.js` `SHELL` and `CACHE` bumped once per task that changes web files).
- Money: `int64_t` cents. Times priced in whole minutes: actual times truncated `t - t % 60` before pricing.
- Day time cost: `(Σ rate_cents_per_h × minutes × factor_permille + 30000) / 60000` per local calendar day (half up, once per day), active factor 1000, unused factor `unused_factor_permille`; then `min(day_cost, time_cap)`.
- Night: local minute-of-day `m` is night iff `night_start > night_end ? (m >= night_start || m < night_end) : (night_start <= m && m < night_end)`; `night_start == night_end` = no night.
- Cap value 0 = no cap (`time_cap_cents_per_day`, `combined_cap_cents_per_day`).
- `late_min = max(0, A1 - (B1 + start_tolerance_min*60)) / 60`; `late_fee = ceil(late_min / 60) * late_fee_cents_per_started_h`; outside both caps.
- `cancelled` booking: price = its `cancel_fee_cents` only (0 for club trips), no tariff needed. `done` without `actual_start`: whole booked time unused.
- Tariff used = version of the car with the largest `valid_from <= booked_start`. None → `no_tariff`.
- Tariff versions append-only: first version per car any `valid_from`; later versions `valid_from >= now`; `valid_from` unique per car; delete only if `valid_from > now`.
- Current tariff (product owner): day 300, night 150, `night_start` 1200, `night_end` 360, km 30, unused 500, time cap 3000, combined cap 5000, late fee 1500 (cents / minutes).
- Data from Phases 1–3 still loads (no `tariffs.jsonl` = no tariffs).

## Review Focus

1. **Return a few seconds into the tolerance edge:** booked end 18:00, returned 18:15:59 → no late fee (minutes truncated). Test: Task 3 `test_late_fee_truncates_seconds`.
2. **Tariff starts after the booking:** a car's only tariff has `valid_from` after `booked_start` → `no_tariff`, never the later tariff. Test: Task 2 `test_tariff_at_ignores_future_version`, Task 5 `test_price_no_tariff`.
3. **Long booking (31 days):** caps per day, combined cap `days × cap`, no overflow. Test: Task 3 `test_long_booking_31_days`.
4. **Bad estimate input:** `end <= start`, `km < 0`, `km > 10000000`, unknown car → 400 / 404; car without tariff → 200 with `status: "no_tariff"`. Test: Task 5 `test_estimate_rejects_bad_input`.
5. **Early start / late return:** active time is `A`, even before `B0`; unused only `B \ A`. Test: Task 3 `test_early_start_counts_active`.

---

## File Structure

```
server/src/core/berlin.{h,c}         NEW: Europe/Berlin UTC offset, local midnight
server/src/core/model.h              tariff_t
server/src/core/codec.{h,c}          tariff codec + validation
server/src/store/db.{h,c}            tariffs table, db_tariff_at, put, delete
server/src/core/pricing.{h,c}        NEW: engine
server/src/http/api_tariff.c         NEW: admin tariff CRUD
server/src/http/api_price.c          NEW: price JSON on bookings, estimate endpoint
server/src/http/api.c, api_internal.h  routes, declarations
server/src/http/api_booking.c, api_admin.c  attach price to booking JSON
server/test/test_berlin.c, test_pricing.c, test_api_tariff.c, test_api_price.c  NEW
web/js/price.js, web/test/price.test.mjs   NEW: price object -> lines
web/js/views/admin_cars.js           tariff list + form
web/js/views/booking_form.js         km field + estimate (layout C)
web/js/views/history.js, admin_bookings.js  price line
docs/spec.md                         §6 cancelled price
```

---

### Task 1: Europe/Berlin time

**Files:**
- Create: `server/src/core/berlin.{h,c}`, `server/test/test_berlin.c`
- Modify: `server/CMakeLists.txt` (`ocs_add_test(test_berlin)`, `berlin.c` in `ocs_core`)

**Interfaces:**
- Produces (`core/berlin.h`):
  ```c
  int64_t berlin_offset(int64_t utc);          /* 3600 (CET) or 7200 (CEST) */
  int64_t berlin_day_start(int64_t utc);       /* UTC instant of local 00:00 of the local day containing utc */
  int64_t berlin_next_day_start(int64_t utc);  /* UTC instant of the following local 00:00 */
  ```
  EU rule: CEST from the last Sunday of March 01:00 UTC (inclusive) to the last Sunday of October 01:00 UTC (exclusive). Civil-from-days arithmetic in integers; no `localtime`, no `TZ`.

- [ ] **Step 1: Write the failing tests** (`test_berlin.c`):
  - `test_offset_around_spring_change`: `berlin_offset(1774745999) == 3600`, `berlin_offset(1774746000) == 7200` (2026-03-29 01:00 UTC).
  - `test_offset_around_autumn_change`: `berlin_offset(1792889999) == 7200`, `berlin_offset(1792890000) == 3600` (2026-10-25 01:00 UTC).
  - `test_day_start_winter`: `berlin_day_start(1793923200) == 1793919600` (2026-11-06 01:00 local → 00:00 local); `berlin_day_start(1793919600) == 1793919600`.
  - `test_short_and_long_day`: `berlin_next_day_start(1774738800) - 1774738800 == 23 * 3600` (2026-03-29); `berlin_next_day_start(1792879200) - 1792879200 == 25 * 3600` (2026-10-25, local midnight = 1792879200).
  - `test_other_years`: change instants for 2027 (28 Mar / 31 Oct) and 2030 (31 Mar / 27 Oct) at 01:00 UTC flip the offset.
- [ ] **Step 2: Run** `cmake --build build && ctest --test-dir build -R test_berlin --output-on-failure` — Expected: compile error.
- [ ] **Step 3: Implement** `core/berlin.c`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Commit** `feat(core): Europe/Berlin offset and local day boundaries`

---

### Task 2: Tariff model, codec and store

**Files:**
- Modify: `server/src/core/model.h`, `server/src/core/codec.{h,c}`, `server/src/store/db.{h,c}`
- Test: `server/test/test_codec.c`, `server/test/test_db.c`

**Interfaces:**
- Produces:
  ```c
  typedef struct {
    int64_t id, car_id, valid_from;
    int64_t rate_day_cents_per_h, rate_night_cents_per_h, rate_km_cents;
    int night_start, night_end;          /* minutes after local midnight, 0..1439 */
    int unused_factor_permille;          /* 0..1000 */
    int64_t time_cap_cents_per_day, combined_cap_cents_per_day, late_fee_cents_per_started_h;
    int64_t created_at;
  } tariff_t;
  cJSON *tariff_to_json(const tariff_t *v);
  int tariff_from_json(const cJSON *j, tariff_t *out);   /* all keys required; -1 if !tariff_valid */
  bool tariff_valid(const tariff_t *t);   /* money fields 0..10000000, night_* 0..1439, factor 0..1000 */
  /* db.h */
  const tariff_t *db_tariff_at(const db_t *db, int64_t car_id, int64_t ts); /* largest valid_from <= ts */
  tariff_t *db_tariff(db_t *db, int64_t id);
  int db_put_tariff(db_t *db, tariff_t *t);      /* id==0: assign max+1 and insert; persist tariffs.jsonl */
  int db_delete_tariff(db_t *db, int64_t id);    /* persist; RAM restored on failure */
  ```
  `db_t` gains `tariff_t *tariffs; size_t n_tariffs, cap_tariffs;`. JSON keys = field names.

- [ ] **Step 1: Write the failing tests:**
  - `test_codec.c` `test_tariff_roundtrip` (the current tariff from Global Constraints, `id 1, car_id 1, valid_from 1793919600`), `test_tariff_rejects_out_of_range` (`night_start 1440`, `unused_factor_permille 1001`, `rate_km_cents -1` → -1), `test_tariff_requires_all_keys`.
  - `test_db.c` `test_tariffs_persist` (2 versions, reload, same), `test_tariff_at_picks_latest_valid` (versions at 100 and 200: ts 150 → 100, ts 200 → 200, ts 99 → NULL), `test_tariff_at_ignores_future_version` (only version at 500: ts 400 → NULL), `test_tariff_at_other_car` (car 2 version ignored), `test_delete_tariff` (gone after reload), `test_load_without_tariffs_file` (Phase 3 data dir loads, `n_tariffs == 0`).
- [ ] **Step 2: Run** `ctest --test-dir build -R 'test_codec|test_db' --output-on-failure` — Expected: compile errors.
- [ ] **Step 3: Implement** using the existing `DEFINE_PUT_ENTITY` / `persist_table` / `load_file` pattern in `db.c`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Commit** `feat(store): versioned tariffs per car`

---

### Task 3: Pricing engine

**Files:**
- Create: `server/src/core/pricing.{h,c}`, `server/test/test_pricing.c`
- Modify: `server/CMakeLists.txt`, `docs/spec.md` §6

**Interfaces:**
- Consumes: Task 1 `berlin_*`, Task 2 `tariff_t`.
- Produces (`core/pricing.h`):
  ```c
  typedef enum { PRICE_OK = 0, PRICE_KM_PENDING, PRICE_NO_TARIFF } price_status_t;
  typedef struct {
    price_status_t status;
    int64_t day_min, night_min;               /* active minutes by rate */
    int64_t unused_day_min, unused_night_min;
    int days;                                 /* local calendar days touched by span */
    int64_t time_cents;                       /* after per-day time cap */
    int64_t time_cap_cut_cents;               /* >= 0: Σ(day cost - capped day cost) */
    int64_t km, km_cents;
    int64_t combined_cap_cut_cents;           /* >= 0 */
    int64_t late_min, late_fee_cents, cancel_fee_cents;
    int64_t total_cents;  /* OK: full total. KM_PENDING: time_cents + late_fee (no km, no combined cap). NO_TARIFF: 0 */
  } price_t;
  /* §6 formula. a0 == 0: never started. km < 0: km unknown (KM_PENDING). */
  void price_compute(const tariff_t *t, int tolerance_min, int64_t b0, int64_t b1,
                     int64_t a0, int64_t a1, int64_t km, price_t *out);
  /* Booking wrapper: cancelled -> cancel_fee only (status OK, no tariff needed); club_trip -> all 0;
   * t == NULL otherwise -> NO_TARIFF; done with km_pending -> km unknown; km = km_end - km_start. */
  void price_booking(const tariff_t *t, const settings_t *s, const booking_t *b, price_t *out);
  ```
  Span = `[min(B0,A0), max(B1,A1))`; a minute is active if in `[A0,A1)`, unused if in `[B0,B1)` and not active. For `reserved`/`active` bookings `price_booking` prices the booked time as fully used with km unknown (`KM_PENDING`); the UI shows prices only for `done`/`cancelled`. Minute-by-minute iteration is acceptable (bounded by booking length).

- [ ] **Step 1: Write the failing tests** (`test_pricing.c`, tariff = current tariff, tolerance 15, `L = 1793919600` = Fri 2026-11-06 00:00 local, `H = 3600`):
  - `test_afternoon_40km`: B = A = L+14H..L+18H, km 40 → `total 2400`, `day_min 240`, `time 1200`, `km_cents 1200`.
  - `test_early_return_unused`: B = L+14H..L+18H, A = L+14H..L+16H, km 40 → `total 2100`, `unused_day_min 120`.
  - `test_full_day_both_caps`: B = A = L..L+24H, km 200 → `time_cents 3000`, `time_cap_cut 2700`, `km_cents 6000`, `combined_cap_cut 4000`, `total 5000`, `days 1`, `day_min 840`, `night_min 600`.
  - `test_late_fee_tolerance`: B1 = L+18H; A1 = L+18H+10*60 → late 0; A1 = L+18H+20*60 → `late_fee 1500`; A1 = L+19H+20*60 → `3000`; late fee added after combined cap.
  - `test_late_fee_truncates_seconds`: A1 = L+18H+15*60+59 → `late_fee 0`.
  - `test_weekend_three_days`: B = A = L+18H .. L+18H+48H, km 1000 → `days 3`, `total 15000`.
  - `test_night_window`: A = L+19H..L+21H → `day_min 60`, `night_min 60`, `time 450`.
  - `test_spring_dst_day`: tariff with caps 0, B = A = 1774738800 .. 1774738800+23H → `night_min 540`, `day_min 840`, `time 5550`, `days 1`.
  - `test_autumn_dst_day`: caps 0, B = A = 1792879200 .. +25H → `night_min 660`, `time 5850`.
  - `test_no_show_all_unused`: a0 = 0, B = L+14H..L+18H, km 0 → `unused_day_min 240`, `total 600`.
  - `test_early_start_counts_active`: B = L+14H..L+18H, A = L+13H..L+18H → `day_min 300`, unused 0.
  - `test_long_booking_31_days`: B = A = L .. L+31*24H, km 5000 → `days 31`, `total 31*5000`.
  - `test_rounding_once_per_day`: A = 1 minute night → `time 3` (150/60 = 2.5 → 3); unused 1 minute night → `time 1` (1.25 → 1).
  - `test_cap_zero_means_none`: caps 0, full day km 200 → `total 5700 + 6000`.
  - `test_booking_cancelled`: state CANCELLED, `cancel_fee_cents 500`, t NULL → status OK, `total 500`.
  - `test_booking_club_trip`: club_trip true, done, late → `total 0`.
  - `test_booking_km_pending`: done, `km_pending` true → `status KM_PENDING`, `total == time_cents + late_fee_cents`, `km_cents 0`.
  - `test_booking_no_tariff`: done, t NULL → `PRICE_NO_TARIFF`.
- [ ] **Step 2: Run** `cmake --build build && ctest --test-dir build -R test_pricing --output-on-failure` — Expected: compile error.
- [ ] **Step 3: Implement** `core/pricing.c` (no I/O, no allocation).
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Spec §6:** after the `total` line add: cancelled bookings cost only `cancel_fee` (no time or km cost); the formula prices `done` bookings, a `done` booking that never started has all booked time unused. Add "Granularity: whole minutes (actual times truncated); time cost rounded half up once per local day; cap 0 = no cap."
- [ ] **Step 6: Commit** `feat(core): pricing engine`

---

### Task 4: Tariff admin API

**Files:**
- Create: `server/src/http/api_tariff.c`, `server/test/test_api_tariff.c`
- Modify: `server/src/http/api.c` (routes), `server/src/http/api_internal.h`, `server/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2.
- Produces routes (admin):
  - `GET /api/admin/cars/{id}/tariffs` → 200 array, newest `valid_from` first; 404 unknown car.
  - `POST /api/admin/cars/{id}/tariffs` body = all `tariff_t` fields except `id`, `car_id`, `created_at` → 201 tariff. 400 `invalid` (missing/out of range); 409 `tariff_in_past`, message `Ein neuer Tarif darf erst ab heute gelten.` when the car already has a version and `valid_from < now`; 409 `duplicate_valid_from`, message `Für diesen Zeitpunkt gibt es schon einen Tarif.`.
  - `DELETE /api/admin/tariffs/{id}` → 200 `{"ok":true}`; 404 unknown; 409 `tariff_active`, message `Ein Tarif, der schon gilt, kann nicht gelöscht werden.` when `valid_from <= now`.

- [ ] **Step 1: Write the failing tests** (`test_api_tariff.c`): `test_first_tariff_may_be_backdated`, `test_later_tariff_in_past_409`, `test_later_tariff_today_ok` (`valid_from == now`), `test_duplicate_valid_from_409`, `test_tariff_invalid_400` (`night_end 1440`, missing key), `test_list_newest_first`, `test_delete_future_tariff`, `test_delete_active_tariff_409`, `test_tariff_routes_admin_only` (user → 403), `test_unknown_car_404`.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_tariff --output-on-failure` — Expected: compile error.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Commit** `feat(server): admin tariff API`

---

### Task 5: Prices on bookings and the estimate endpoint

**Files:**
- Create: `server/src/http/api_price.c`, `server/test/test_api_price.c`
- Modify: `server/src/http/api_booking.c`, `server/src/http/api_admin.c`, `server/src/http/api.c`, `server/src/http/api_internal.h`, `server/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 `db_tariff_at`, Task 3 `price_booking`, `price_compute`.
- Produces:
  - `cJSON *api_booking_json(app_t *a, const booking_t *b);` — `booking_to_json` plus key `"price"` = `price_to_json`. Replaces every `booking_to_json` call in `api_booking.c` / `api_admin.c` responses.
  - `cJSON *price_to_json(const price_t *p, const tariff_t *t);` — keys: `status` (`"ok"|"km_pending"|"no_tariff"`), every `price_t` number field by its name, and `"tariff"` = `tariff_to_json(t)` or `null`.
  - `GET /api/price-estimate?car=<id>&start=<s>&end=<e>&km=<k>[&club=1]` (member) → 200 `price_to_json` of `price_compute(t, tol, s, e, s, e, km)` with `t = db_tariff_at(car, s)` (club → all 0, status ok). 400 `invalid`: missing/non-integer, `end <= start`, `km < 0`, `km > 10000000`, `end - start > 90 * 86400`. 404 unknown or inactive car.

- [ ] **Step 1: Write the failing tests** (`test_api_price.c`, car 1 with the current tariff `valid_from` before all bookings):
  - `test_history_has_price`: done booking 14:00–18:00, km 1000/1040 → `GET /api/bookings?scope=history` item `price.total_cents == 2400`, `price.status == "ok"`.
  - `test_admin_bookings_have_price`.
  - `test_price_km_pending`: done, `km_pending` → `status "km_pending"`.
  - `test_price_no_tariff`: tariff `valid_from` after `booked_start` → `status "no_tariff"`.
  - `test_cancelled_price_is_fee`: cancelled, `cancel_fee_cents 500` → `total_cents 500`.
  - `test_estimate_basic`: 14:00–18:00, km 15 → `total_cents 1650`, `tariff.rate_day_cents_per_h 300`.
  - `test_estimate_club`: `club=1` → `total_cents 0`.
  - `test_estimate_rejects_bad_input`: `end <= start`, `km=-1`, `km=10000001`, missing `km`, unknown car → 400/400/400/400/404; car without tariff → 200 `status "no_tariff"`.
  - `test_estimate_requires_login` → 401.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_price --output-on-failure` — Expected: compile error.
- [ ] **Step 3: Implement**; the existing tests that compare booking JSON must still pass (the extra key is additive).
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass; `cmake --build build-fuzz --target fuzz_api && setarch -R ./build-fuzz/server/fuzz_api -max_total_time=60` — no crash.
- [ ] **Step 5: Commit** `feat(server): booking prices and price estimate`

---

### Task 6: Web — price display helper, history and admin bookings

**Files:**
- Create: `web/js/price.js`, `web/test/price.test.mjs`
- Modify: `web/js/views/history.js`, `web/js/views/admin_bookings.js`, `web/sw.js`

**Interfaces:**
- Consumes: Task 5 `price` JSON.
- Produces (`web/js/price.js`):
  - `priceSummary(price)` → string: `ok` → `formatEuro(total_cents)`; `km_pending` → `` `${formatEuro(total_cents)} + km (km ausstehend)` ``; `no_tariff` → `Kein Tarif`.
  - `priceLines(price)` → array of `{ label, cents }` for the breakdown, only non-zero lines, in this order: `` `${h} h Tag × ${formatEuro(rate_day)}` ``, `… h Nacht × …`, `… h ungenutzt Tag × … × 50 %`, `… h ungenutzt Nacht …`, `` `${km} km × ${formatEuro(rate_km)}` ``, `Deckel` (negative, time cap cut + combined cap cut), `Verspätung` (late fee), `Stornogebühr`. Hours shown as `h:mm` when not whole (`2:30 h`).
  - `priceNote(tariff)` → `` `Nicht genutzte Zeit ${factor/10} %, Verspätung ${formatEuro(late_fee)} je angefangene Stunde.` ``

- [ ] **Step 1: Write the failing tests** (`price.test.mjs`): `priceSummary` for the three statuses; `priceLines` for the afternoon (2 lines), full day with caps (`Deckel -67,00 €`: 2700 + 4000), early return (unused line), cancelled (only `Stornogebühr`), 90 min night (`1:30 h Nacht`); `priceNote` with the current tariff.
- [ ] **Step 2: Run** `(cd web && node --test test/)` — Expected: FAIL (module missing).
- [ ] **Step 3: Implement** `price.js`; history card line `Preis: <priceSummary>` for `done`/`cancelled`; admin bookings row gets the same text. Add `price.js` to `SHELL`, bump `CACHE`.
- [ ] **Step 4: Run** `(cd web && node --test test/)` — all pass.
- [ ] **Step 5: Commit** `feat(web): prices in history and admin bookings`

---

### Task 7: Web — booking form estimate and admin tariffs

**Files:**
- Modify: `web/js/views/booking_form.js`, `web/js/views/admin_cars.js`, `web/sw.js`

**Interfaces:**
- Consumes: Task 4 routes, Task 5 estimate endpoint, Task 6 `priceLines`, `priceNote`, `priceSummary`.

- [ ] **Step 1: Booking form** (design "layout C"): field `Geplante Strecke` (km, `inputmode="numeric"`, default `15`) under start/end; below it `Voraussichtlich <priceSummary>` and a `Details` toggle that shows `priceLines` as a two-column list plus `priceNote`. Fetch `/api/price-estimate` on load and whenever start, end, km or the club checkbox change (debounced ~300 ms; ignore stale responses). Invalid input (end ≤ start, empty km) hides the estimate. Extension shows the estimate for the new start/end. Planned km are not sent with the booking.
- [ ] **Step 2: Admin cars**: per car a `Tarife` section: list newest first (`ab <formatDateTime(valid_from)>: Tag 3,00 €/h, Nacht 1,50 €/h (20:00–06:00), 0,30 €/km, ungenutzt 50 %, Deckel 30,00 € / 50,00 € pro Tag, Verspätung 15,00 €/h`), `Löschen` only for future versions; form `Neuer Tarif ab` (date + time) prefilled with the newest version's values (empty for the first), money via `parseEuroToCents`, night times as `HH:MM`, factor as percent. Server error messages shown as returned.
- [ ] **Step 3: Bump `CACHE`** in `web/sw.js`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure && (cd web && node --test test/)` — all pass. Manual: in the local setup (`/home/michael/ocs-dev`), add the current tariff to the test car with a backdated `valid_from`, book 14:00–18:00 → estimate `16,50 €`, Details shows `4 h Tag × 3,00 €` and `15 km × 0,30 €`; history shows prices for past trips.
- [ ] **Step 5: Commit** `feat(web): price estimate when booking, tariff admin`

---

### Final verification

- [ ] `cmake --build build && ctest --test-dir build --output-on-failure`, `(cd web && node --test test/)`, `cmake --build build-fuzz` and 60 s runs of `fuzz_api` and `fuzz_teltonika` under `setarch -R`. Expected: all green, no fuzz crash. Then superpowers:finishing-a-development-branch; after the merge mark Phase 4 done in `docs/spec.md` §15.
