# User Tariffs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tariffs become named, global, versioned price lists; each user carries dated tariff assignments; every price (estimate, booking, reminder, billing) is computed with the booking user's tariff at `booked_start`. Club trips stay 0 €.

**Architecture:** Three tables replace the per-car tariff: `tariffs` (name), `tariff_versions` (the values, append-only by `valid_from`, what `tariff_t` holds today) and `user_tariffs` (dated assignments). One lookup, `db_tariff_at(db, user_id, ts)`, composes assignment-at-ts and version-at-ts; the four pricing call sites switch from `car_id` to `user_id` and nothing in `core/pricing.c` changes. `http/api_tariff.c` is rewritten for the new admin endpoints; the admin web gets a "Tarife" page and a tariff select on the user form.

**Tech Stack:** unchanged: C11, CMake, Mongoose, cJSON, Unity, vanilla JS (ES2020), `node --test`.

**Spec:** `docs/superpowers/specs/2026-11-09-user-tariffs-design.md` (binding), `docs/spec.md` §4, §6, §9, §10, §13, §15 (updated in Task 7).

**Branch:** `user-tariffs`.

**Out of scope:** per-car prices, data migration (app not deployed), pricing-engine changes, member-visible tariff names.

## Builder calls (fixed here so tasks agree; change in review if wrong)

1. **Names:** `tariff_t` becomes the named tariff `{id, name[64], created_at}`. The values struct is renamed `tariff_version_t` (field `tariff_id` replaces `car_id`); `tariff_valid` becomes `tariff_version_valid`. Reusing the old name makes every stale call site a compile error under `-Werror`.
2. **Files:** `data/tariffs.jsonl` (named tariffs), `data/tariff_versions.jsonl`, `data/user_tariffs.jsonl`.
3. **Deleting a tariff** writes `tariff_versions.jsonl` first, then `tariffs.jsonl`; RAM restored on failure. A crash in between leaves orphan versions, which load fine and are invisible (no tariff references them).
4. **Editing an older assignment** (not the user's newest): 409 `assignment_not_newest` „Nur die neueste Tarifzuordnung kann geändert werden.“
5. **Assignment duplicate `valid_from`**: 409 `duplicate_valid_from` „Für diesen Zeitpunkt gibt es schon eine Zuordnung.“
6. **Unknown `tariff_id`** in any body is 400 `invalid` (same as every other bad field).
7. **`price.tariff`** keeps the full version object including `id` and `tariff_id`; no name anywhere on member routes.
8. **Export key:** `tariffs: [{id, tariff_name, valid_from}]` newest first.
9. **`GET /api/admin/tariffs`** returns tariffs in id order, each with `versions` newest first.
10. **User creation**: the user row is written first, then the assignment; if the assignment write fails the response is 500 `storage` and the user exists without an assignment (a supported state).

## Global Constraints

- All existing Global Constraints hold: `int64_t` UTC seconds, money `int64_t` cents, no float, German UI / English code, fixed `char[]` reject-not-truncate, `-Wall -Wextra -Werror`, ASan/UBSan tests, db copy-before-modify / RAM restored on failed persist, every changed web file listed in `web/sw.js` `SHELL`, `CACHE` bumped once per task that changes web files (`ocs-v18` → `ocs-v19` in Task 5, `ocs-v20` in Task 6).
- Build/test from repo root: `cmake --build build -j && ctest --test-dir build --output-on-failure`; web: `node --test web/test/`.
- Test epoch constant in API tests is `T = 1774742400`; `DAY = 86400`, `H = 3600`.
- Standard version values (the Leapmotor tariff): `rate_day_cents_per_h 300`, `rate_night_cents_per_h 150`, `rate_km_cents 30`, `night_start 1200`, `night_end 360`, `unused_factor_permille 500`, `time_cap_cents_per_day 3000`, `combined_cap_cents_per_day 5000`, `late_fee_cents_per_started_h 1500`.
- Commit messages: Conventional Commits, ending with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

## Review Focus

1. A tariff name of only spaces, or with surrounding spaces: trimmed before uniqueness and length checks; empty after trim is 400. (Task 2)
2. `PUT /api/admin/user-tariffs/{id}` changing only `tariff_id`: the billed check must still run with the unchanged `valid_from`. (Task 3)
3. A member without any assignment calls the estimate or lists history: `status: "no_tariff"`, never a 500. (Task 1)
4. An admin records a past trip for a member whose first assignment starts later: the billing draft lists it under `no_tariff`, and backdating the assignment through the repair path clears the blocker. (Task 4)
5. `GET /api/admin/users` reports `tariff_id` of the assignment in force now, so a future-dated switch does not show yet. (Task 3)

---

### Task 1: Storage model — named tariffs, versions, assignments

**Files:**
- Modify: `server/src/core/model.h` (tariff structs), `server/src/core/codec.h`, `server/src/core/codec.c` (tariff section ~479–531), `server/src/core/pricing.h`, `server/src/core/pricing.c` (typedef rename only), `server/src/store/db.h`, `server/src/store/db.c`
- Modify (call sites): `server/src/http/api_price.c:44,85` (`api_booking_json`, `h_price_estimate`), `server/src/http/api_billing.c:94`, `server/src/http/api_notify.c:62`
- Modify: `server/src/http/api_tariff.c` (old handlers removed; file keeps only the include block and a comment), `server/src/http/api_internal.h` (remove the three `h_admin_tariff*` declarations), `server/src/http/api.c:64-66` (remove the three routes)
- Modify tests: `server/test/helpers.h`, `server/test/helpers.c`, `server/test/test_db.c:470-545`, `server/test/test_codec.c:509-560`, `server/test/test_pricing.c` (rename only), `server/test/test_api_price.c`, `server/test/test_api_billing.c`, `server/test/test_api_notify.c`, `server/test/test_api_tariff.c` (emptied to a `main` with `UNITY_BEGIN(); return UNITY_END();`)

**Interfaces:**
- Produces (model.h):
  ```c
  typedef struct { int64_t id; char name[64]; int64_t created_at; } tariff_t;
  typedef struct {
    int64_t id, tariff_id, valid_from;
    int64_t rate_day_cents_per_h, rate_night_cents_per_h, rate_km_cents;
    int night_start, night_end; int unused_factor_permille;
    int64_t time_cap_cents_per_day, combined_cap_cents_per_day, late_fee_cents_per_started_h;
    int64_t created_at;
  } tariff_version_t;
  typedef struct { int64_t id, user_id, tariff_id, valid_from, created_at, created_by; } user_tariff_t;
  ```
- Produces (codec.h): `cJSON *tariff_to_json(const tariff_t *)`, `int tariff_from_json(const cJSON *, tariff_t *)` (name required, 1..63 bytes, no trimming in codec), `bool tariff_version_valid(const tariff_version_t *)`, `cJSON *tariff_version_to_json(const tariff_version_t *)`, `int tariff_version_from_json(const cJSON *, tariff_version_t *)` (keys as today with `tariff_id` instead of `car_id`), `cJSON *user_tariff_to_json(const user_tariff_t *)`, `int user_tariff_from_json(const cJSON *, user_tariff_t *)` (all six keys required).
- Produces (db.h): arrays `tariffs/n_tariffs/cap_tariffs` (tariff_t), `versions/n_versions/cap_versions` (tariff_version_t), `user_tariffs/n_user_tariffs/cap_user_tariffs`; functions
  ```c
  tariff_t *db_tariff(db_t *db, int64_t id);
  tariff_t *db_tariff_by_name(db_t *db, const char *name);               /* exact match */
  tariff_version_t *db_version(db_t *db, int64_t id);
  const tariff_version_t *db_version_at(const db_t *db, int64_t tariff_id, int64_t ts); /* largest valid_from <= ts */
  user_tariff_t *db_user_tariff(db_t *db, int64_t id);
  const user_tariff_t *db_user_tariff_at(const db_t *db, int64_t user_id, int64_t ts);  /* largest valid_from <= ts */
  const user_tariff_t *db_user_tariff_newest(const db_t *db, int64_t user_id);          /* largest valid_from, NULL if none */
  const tariff_version_t *db_tariff_at(const db_t *db, int64_t user_id, int64_t ts);    /* assignment at ts, then its version at ts */
  int db_put_tariff(db_t *db, tariff_t *t);           /* id==0 insert, else replace */
  int db_put_version(db_t *db, tariff_version_t *v);
  int db_put_user_tariff(db_t *db, user_tariff_t *u);
  int db_delete_tariff(db_t *db, int64_t id);         /* also removes its versions; -1 unknown/persist failed */
  int db_delete_version(db_t *db, int64_t id);
  int db_delete_user_tariff(db_t *db, int64_t id);
  ```
- Produces (pricing.h): `price_compute(const tariff_version_t *t, …)`, `price_booking(const tariff_version_t *t, …)`; `price_to_json(const price_t *, const tariff_version_t *)` in api_internal.h.
- Produces (helpers.h, no asserts, id or -1):
  ```c
  int64_t th_add_tariff(app_t *a, const char *name);                                   /* named tariff, no version */
  /* One version with the standard rates (300/150/30, night 1200–360, unused 500) and the given caps/late fee. */
  int64_t th_add_version(app_t *a, int64_t tariff_id, int64_t valid_from, int64_t time_cap, int64_t combined_cap, int64_t late_fee);
  int64_t th_assign_tariff(app_t *a, int64_t user_id, int64_t tariff_id, int64_t valid_from); /* created_by 0 */
  ```

- [ ] **Step 1: Write the failing db tests** in `server/test/test_db.c`, replacing the block from `mktariff` to `test_delete_tariff`:

```c
static tariff_version_t mkversion(int64_t tariff_id, int64_t valid_from) { /* standard values, created_at 50 */ }
static tariff_t mkdef(const char *name) { /* created_at 50 */ }

void test_tariff_tables_persist(void) {
  /* put def "Standard", two versions (valid_from 100, 200; second rate_km_cents 40), one assignment (user 7, valid_from 100, created_by 1);
     db_free + db_load; assert n_tariffs 1, n_versions 2, n_user_tariffs 1; db_tariff(1)->name "Standard";
     db_version(2)->rate_km_cents 40; db_user_tariff(1)->created_by 1 */
}
void test_version_at_picks_latest_valid(void)  { /* versions 100,200 on tariff 1: at 150 -> 100, at 200 -> 200, at 99 -> NULL */ }
void test_version_at_other_tariff(void)        { /* version on tariff 2 only: db_version_at(1,150) NULL, (2,150) non-NULL */ }
void test_user_tariff_at_boundaries(void)      { /* assignments user 7: tariff 1 from 100, tariff 2 from 300: at 99 NULL, at 100 -> tariff 1, at 299 -> 1, at 300 -> 2; user 8 NULL; db_user_tariff_newest(7)->tariff_id 2 */ }
void test_tariff_at_composes(void) {
  /* tariff 1 version from 100; tariff 2 version from 500; user 7: tariff 1 from 100, tariff 2 from 300.
     db_tariff_at(7,200)->tariff_id 1; db_tariff_at(7,400) NULL (tariff 2 has no version yet); db_tariff_at(7,500)->tariff_id 2;
     db_tariff_at(8,500) NULL */
}
void test_delete_tariff_removes_versions(void) { /* defs 1,2 with versions 2 each; db_delete_tariff(1) == 0; db_tariff(1) NULL; n_versions 2, all tariff_id 2; reload: same; db_delete_tariff(99) == -1 */ }
void test_delete_version_and_assignment(void)  { /* db_delete_version / db_delete_user_tariff: 0 on known id, -1 on 99, gone after reload */ }
```

Register all in `main`, replacing the old tariff `RUN_TEST` lines.

- [ ] **Step 2: Write the failing codec tests** in `server/test/test_codec.c`, replacing `mktariff`/`test_tariff_roundtrip`/`test_tariff_rejects_out_of_range`: `test_tariff_roundtrip` (name "Standard", id 3, created_at 9), `test_tariff_rejects_empty_name` (`""` → -1; 63 'a' ok; 64 'a' → -1), `test_tariff_version_roundtrip` (today's assertions, key `tariff_id`), `test_tariff_version_rejects_out_of_range` (today's, via `tariff_version_valid`), `test_user_tariff_roundtrip` (all six fields), `test_user_tariff_requires_all_keys` (object without `created_by` → -1).

- [ ] **Step 3: Run the two test binaries to verify they fail to compile**

Run: `cmake --build build -j 2>&1 | grep -m3 error`
Expected: errors about `tariff_version_t` / `mkdef` undeclared.

- [ ] **Step 4: Change the model** in `server/src/core/model.h`: replace the `tariff_t` block with the three structs from Interfaces (keep the comment "Versioned price list …" on `tariff_version_t`, reworded to "of one tariff").

- [ ] **Step 5: Codec** in `codec.h`/`codec.c`: implement the six functions from Interfaces following the existing `PUT`/`TRY`/`get_str`/`put_str` pattern; `tariff_from_json` fails on empty name (`get_str` already rejects ≥ 64 bytes).

- [ ] **Step 6: Pricing rename**: `sed -i 's/\btariff_t\b/tariff_version_t/g'` over `server/src/core/pricing.h`, `server/src/core/pricing.c`, `server/test/test_pricing.c`; `tariff_valid` → `tariff_version_valid` wherever referenced.

- [ ] **Step 7: Store** in `db.h`/`db.c`: arrays, loaders (`DEFINE_LOADER_X` for the three tables; load order after `damage_reports.jsonl`: `tariffs.jsonl`, `tariff_versions.jsonl`, `user_tariffs.jsonl`), `db_free` frees, `DEFINE_PUT_ENTITY` for the three puts, lookups from Interfaces, and the three deletes. Factor the body of today's `db_delete_tariff` into `static int delete_one(db_t *db, void **arr, size_t *n, size_t esz, int64_t id, const char *file, to_json_fn)` (id is the first `int64_t` of every entity) and use it for all three; `db_delete_tariff` additionally filters versions with that `tariff_id` out (copy, persist `tariff_versions.jsonl`, then `delete_one` on the def; restore both on failure).

- [ ] **Step 8: Call sites**: `api_price.c:44` → `db_tariff_at(&a->db, b->user_id, b->booked_start)`; `api_billing.c:94` → `db_tariff_at(db, b->user_id, b->booked_start)`; `api_notify.c:62` → `db_tariff_at(&a->db, b.user_id, b.booked_start)`; `price_to_json`/`api_booking_json` signatures take `const tariff_version_t *`. In `h_price_estimate` (`api_price.c:75-96`): drop the `car` query parameter and the `db_car` lookup, use `db_tariff_at(&a->db, ctx->user->id, start)`. Update the comment on `api_booking_json` in `api_internal.h` ("tariff of the user at booked_start").

- [ ] **Step 9: Remove the old admin tariff API**: delete the three handler bodies from `api_tariff.c` (keep includes and a comment `/* admin tariffs: Task 2 */`), their declarations in `api_internal.h`, their three rows in `api.c`. Reduce `server/test/test_api_tariff.c` to includes plus `int main(void) { UNITY_BEGIN(); return UNITY_END(); }`.

- [ ] **Step 10: Test helpers**: add the three functions from Interfaces to `helpers.h`/`helpers.c` (`th_add_version` sets `created_at = valid_from`).

- [ ] **Step 11: Fixture rewrites** so the existing tests express the same scenarios through users:
  - `test_api_price.c`: `setUp` creates tariff `std = th_add_tariff(a, "Standard")`, `th_add_version(a, std, T - 100*DAY, 3000, 5000, 1500)`, `th_assign_tariff(a, u1, std, T - 100*DAY)`; delete `add_tariff`. `est()` loses the `car` argument (`/api/price-estimate?start=…&end=…%s`). In `test_estimate_rejects_bad_input` drop the three car assertions (999, inactive car2, car2 no-tariff) and instead add a user `u2 = th_add_user(a, "bo@x.de", NULL, ROLE_USER)` with a session and assert their estimate is `status "no_tariff"` with `tariff` null (Review Focus 3); also assert `GET /api/bookings?scope=history` for `u2` with one planted `done` booking is 200 with `price.status "no_tariff"`.
  - `test_api_billing.c`: `add_car(name, tariff)` becomes `add_car(name)`; `setUp` creates `std` with `th_add_version(a, std, 1, 0, 0, 1500)` (no caps, as today) and assigns `uz` and `ua` at `valid_from 1`. `test_finalize_blocked_no_tariff` uses a new user `ub = th_add_user(a, "bo@x.de", …)` without assignment on `car1` instead of `car2`.
  - `test_api_notify.c` `test_late_reminder_fee_from_tariff`: `th_add_tariff` + `th_add_version(a, t, 1, 0, 0, 300)` + `th_assign_tariff(a, u1, t, 1)` instead of the `tariff_t` block.

- [ ] **Step 12: Build and run everything**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: all tests pass, including the new `test_db` and `test_codec` cases.

- [ ] **Step 13: Commit**

```bash
git add server/
git commit -m "feat(store): named tariffs, versions and user assignments replace car tariffs"
```

---

### Task 2: Admin API for tariffs and versions

**Files:**
- Modify: `server/src/http/api_tariff.c`, `server/src/http/api_internal.h`, `server/src/http/api.c` (routes)
- Test: `server/test/test_api_tariff.c`

**Interfaces:**
- Consumes: Task 1 db functions and helpers.
- Produces routes (all `MIN_ADMIN`, `BODY_JSON`):
  ```
  GET    /api/admin/tariffs                 h_admin_tariffs_get
  POST   /api/admin/tariffs                 h_admin_tariff_post        {name}
  PUT    /api/admin/tariffs/*               h_admin_tariff_put         {name}
  DELETE /api/admin/tariffs/*               h_admin_tariff_delete
  POST   /api/admin/tariffs/*/versions      h_admin_version_post       {valid_from, rate_… (today's body)}
  DELETE /api/admin/tariff-versions/*       h_admin_version_delete
  ```
- Produces: `cJSON *tariff_api_json(const db_t *db, const tariff_t *t)` = `tariff_to_json` plus `versions` (array, newest first) — used by Task 3's tests only through the routes.
- Error codes: 400 `invalid`; 404 `not_found`; 409 `duplicate_name` „Diesen Tarifnamen gibt es schon.“; 409 `tariff_in_use` „Dieser Tarif ist Nutzern zugeordnet.“; versions as today: 409 `duplicate_valid_from` „Für diesen Zeitpunkt gibt es schon einen Tarif.“, 409 `tariff_in_past` „Ein neuer Tarif darf erst ab heute gelten.“, 409 `tariff_active` „Ein Tarif, der schon gilt, kann nicht gelöscht werden.“

- [ ] **Step 1: Write the failing tests** in `server/test/test_api_tariff.c` (reuse today's `call`/`adm_free`/`num`/`tbody` helpers; `setUp` as today without the car):

```c
static int64_t post_tariff(const char *name);   /* POST /api/admin/tariffs {"name":…} -> 201, returns id, asserts versions is empty array */
static const char *vpath(int64_t tid);          /* "/api/admin/tariffs/<tid>/versions" */
static int64_t post_version(int64_t tid, int64_t valid_from); /* 201; asserts tariff_id == tid, created_at == T */

void test_create_and_list_tariffs(void)      { /* post "Standard", "Reduziert"; GET -> array of 2 in id order; names; versions [] */ }
void test_name_trimmed_and_unique(void)      { /* post "  Standard " -> name "Standard"; post "Standard" -> 409 duplicate_name + message; post "   " -> 400 invalid; post 64 'a' -> 400; post {} -> 400 */ }
void test_rename(void)                        { /* PUT {"name":"Normal"} -> 200 name Normal; PUT to existing other name -> 409 duplicate_name; PUT own name unchanged -> 200; PUT 999 -> 404 */ }
void test_delete_unassigned_removes_versions(void) { /* tariff with 2 versions (T-DAY, T+DAY): DELETE -> 200 ok; n_tariffs 0, n_versions 0; DELETE again -> 404 */ }
void test_delete_assigned_409(void)           { /* th_assign_tariff(u1, tid, T + DAY) (future only): DELETE -> 409 tariff_in_use + message; still present */ }
void test_first_version_may_be_backdated(void)   { /* post_version(T - 100*DAY); n_versions 1 */ }
void test_later_version_in_past_409(void)        { /* as today's test_later_tariff_in_past_409 */ }
void test_later_version_today_ok(void)           { /* as today */ }
void test_version_duplicate_valid_from_409(void) { /* as today, duplicate wins over past */ }
void test_version_invalid_400(void)              { /* night_end 1440; {"valid_from":1}; NULL body */ }
void test_version_body_ids_ignored(void)         { /* body with id/tariff_id/created_at: response has tariff_id == tid, created_at T, id != 99 */ }
void test_versions_listed_newest_first(void)     { /* 3 versions; GET /api/admin/tariffs -> tariffs[0].versions valid_from order T+2DAY, T+DAY, T-100DAY */ }
void test_delete_future_version(void)            { /* as today's test_delete_future_tariff on /api/admin/tariff-versions/<id> */ }
void test_delete_active_version_409(void)        { /* valid_from == T -> 409 tariff_active + message */ }
void test_unknown_tariff_404(void)               { /* POST /api/admin/tariffs/999/versions -> 404; DELETE /api/admin/tariffs/999 -> 404; DELETE /api/admin/tariff-versions/999 -> 404 */ }
void test_tariff_routes_admin_only(void)         { /* every route with tu1 -> 403 */ }
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j && ctest --test-dir build -R test_api_tariff --output-on-failure`
Expected: 404 `not_found` on every call (routes missing) → assertion failures.

- [ ] **Step 3: Implement the handlers** in `api_tariff.c`: name handling = copy the JSON string (`api_get_str(body, "name", 64)`), trim ASCII spaces both ends into a `char name[64]`, reject empty; uniqueness via `db_tariff_by_name`. `h_admin_version_post` is today's `h_admin_tariff_post` with `tariff_id` from `api_segment_id(req->path, "/api/admin/tariffs/", &tid)` and `db_tariff(tid)` existence (404). `h_admin_tariff_delete` refuses with `tariff_in_use` when any `user_tariffs` row has that `tariff_id`. Declare in `api_internal.h`, add the six routes in `api.c`.

- [ ] **Step 4: Run tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add server/
git commit -m "feat(api): admin endpoints for named tariffs and their versions"
```

---

### Task 3: Assignments — user creation, switch, repair path, listing, export

**Files:**
- Modify: `server/src/http/api_tariff.c` (assignment handlers), `server/src/http/api_internal.h`, `server/src/http/api.c`, `server/src/http/api_admin.c` (`h_admin_users_get`, `h_admin_user_post`, `h_admin_user_export`)
- Test: `server/test/test_api_tariff.c`, `server/test/test_api_admin.c`

**Interfaces:**
- Consumes: Task 1 db functions; `db_user_tariff_newest`.
- Produces routes (`MIN_ADMIN`):
  ```
  GET    /api/admin/users/*/tariffs       h_admin_user_tariffs_get   -> [user_tariff_to_json …] newest first
  POST   /api/admin/users/*/tariffs       h_admin_user_tariff_post   {tariff_id, valid_from} -> 201 user_tariff json
  PUT    /api/admin/user-tariffs/*        h_admin_user_tariff_put    {tariff_id?, valid_from?} -> 200 user_tariff json
  DELETE /api/admin/user-tariffs/*        h_admin_user_tariff_delete -> 200 {"ok":true}
  ```
- `POST /api/admin/users` body gains required `tariff_id`; `GET /api/admin/users` rows gain `tariff_id` (assignment at `req->now`, else 0); export gains `tariffs` (Builder call 8).
- Error codes: 409 `assignment_in_past` „Ein Tarifwechsel darf erst ab heute gelten.“; 409 `duplicate_valid_from` „Für diesen Zeitpunkt gibt es schon eine Zuordnung.“; 409 `assignment_not_newest` (Builder call 4); 409 `assignment_billed` „Es gibt schon abgerechnete Fahrten in diesem Zeitraum.“; 400 `invalid` for unknown `tariff_id`, empty PUT body, or `valid_from` not greater than the previous assignment's.
- Check order (POST): 404 user → body valid (`tariff_id` exists, `valid_from` integer) → duplicate → in past. (PUT/DELETE): 404 → not newest → body valid → `valid_from` vs previous (equal: duplicate; smaller: invalid) → billed.
- Billed check: any booking of the user with `invoice_id != 0 && booked_start >= min(old.valid_from, new.valid_from)` (DELETE: `>= old.valid_from`).

- [ ] **Step 1: Write the failing tests** in `test_api_tariff.c` (fixture: `std` and `red` tariffs each with a version at `T - 100*DAY`; `u1` assigned `std` at `T - 50*DAY` through `th_assign_tariff`):

```c
void test_switch_future_ok_and_listed(void)   { /* POST users/u1/tariffs {red, T+DAY} -> 201 created_by adm, created_at T; GET -> 2 rows, [0].valid_from T+DAY */ }
void test_switch_today_ok(void)               { /* valid_from T -> 201 */ }
void test_switch_in_past_409(void)            { /* T-1 -> 409 assignment_in_past + message */ }
void test_switch_duplicate_409(void)          { /* T-50*DAY (the existing one) -> 409 duplicate_valid_from + „…schon eine Zuordnung.“ (duplicate wins over past) */ }
void test_switch_unknown_tariff_400(void)     { /* tariff_id 999 -> 400 invalid; missing valid_from -> 400; user 999 -> 404 */ }
void test_repair_backdates_newest(void)       { /* PUT user-tariffs/<first> {valid_from: T-200*DAY} -> 200; GET shows it; db_tariff_at(u1, T-150*DAY) non-NULL */ }
void test_repair_changes_tariff_only(void)    { /* PUT {tariff_id: red} -> 200, valid_from unchanged (T-50*DAY) */ }
void test_repair_rejects_older(void)          { /* add switch at T+DAY; PUT on the first -> 409 assignment_not_newest + message */ }
void test_repair_valid_from_vs_previous(void) { /* first at T-50*DAY, newest at T+DAY: PUT newest {valid_from: T-50*DAY} -> 409 duplicate_valid_from; {valid_from: T-60*DAY} -> 400 invalid; {valid_from: T-49*DAY} -> 200 */ }
void test_repair_empty_body_400(void)         { /* {} and NULL -> 400 invalid; {tariff_id: 999} -> 400 */ }
void test_repair_billed_409(void) {
  /* plant done booking of u1 at T-10*DAY with invoice_id 1; PUT first {valid_from: T-20*DAY} -> 409 assignment_billed + message;
     PUT {tariff_id: red} (valid_from stays T-50*DAY <= T-10*DAY) -> 409 (Review Focus 2);
     set booking.invoice_id = 0 -> same PUT -> 200 */
}
void test_delete_newest_and_only(void)        { /* DELETE first (in force, only one) -> 200 ok; GET -> []; db_tariff_at(u1, T) NULL; DELETE again -> 404 */ }
void test_delete_rejects_older_and_billed(void) { /* with a second at T+DAY: DELETE first -> 409 assignment_not_newest; delete second -> 200; plant invoiced booking at T-10*DAY: DELETE first -> 409 assignment_billed */ }
void test_assignment_routes_admin_only(void)  { /* all four with tu1 -> 403 */ }
```

And in `test_api_admin.c`: `setUp` adds `std = th_add_tariff(a, "Standard")` with a version at `1`; `user_body(email, role)` gains `"tariff_id":<std>`; new tests:

```c
void test_user_post_requires_tariff(void)   { /* body without tariff_id -> 400 invalid; tariff_id 999 -> 400 invalid; no user created (n_users unchanged) */ }
void test_user_post_assigns_tariff(void)    { /* 201; db_user_tariff_newest(new id): tariff_id std, valid_from T, created_by adm */ }
void test_users_list_has_tariff_id(void)    { /* GET /api/admin/users: new user row tariff_id == std; u1 (no assignment) tariff_id 0;
                                                 th_assign_tariff(u1, std, T + DAY) -> still 0 (Review Focus 5); th_assign_tariff(u1, std, T) -> std */ }
void test_user_put_ignores_tariff_id(void)  { /* PUT with tariff_id 999 -> 200; assignments unchanged */ }
void test_export_lists_tariffs(void)        { /* assign u1 std at 5 then red at T+DAY; export.tariffs length 2, [0].tariff_name "Reduziert", [0].valid_from T+DAY, [1].id present */ }
void test_me_has_no_tariff(void)            { /* GET /api/me as u1 with assignment: response JSON has no key "tariff", "tariff_id" or "tariffs" */ }
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j && ctest --test-dir build -R "test_api_tariff|test_api_admin" --output-on-failure`
Expected: failures (404 on new routes; 201 where 400 expected for missing `tariff_id`).

- [ ] **Step 3: Implement** the four handlers in `api_tariff.c` per Interfaces; `created_by = ctx->user->id`. In `api_admin.c`: `h_admin_user_post` reads `tariff_id` (`api_get_int`, `db_tariff` must exist) before writing the user, writes the assignment `{user_id, tariff_id, valid_from = now, created_at = now, created_by = ctx->user->id}` after `db_put_user` (Builder call 10); `h_admin_users_get` adds `tariff_id`; `h_admin_user_export` adds `tariffs` (`static cJSON *export_assignment(const db_t *, const user_tariff_t *)`, newest first via `qsort` by `valid_from` desc). Routes and declarations as in Task 2.

- [ ] **Step 4: Run tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add server/
git commit -m "feat(api): user tariff assignments with forward switch and repair path"
```

---

### Task 4: Pricing through the user's tariff — cross-cutting tests

**Files:**
- Test: `server/test/test_api_price.c`, `server/test/test_api_billing.c`, `server/test/test_api_notify.c`

**Interfaces:**
- Consumes: Tasks 1–3. No production code expected; a failing test here is a bug in those tasks and is fixed where it lives.

- [ ] **Step 1: Write the tests**

`test_api_price.c` (fixture from Task 1; add `red = th_add_tariff(a, "Reduziert")` with `th_add_version(a, red, T - 100*DAY, 0, 0, 0)` but `rate_day_cents_per_h` edited to 150 via `db_version(...)->rate_day_cents_per_h = 150; db_put_version(...)`, and user `u2` assigned `red`):

```c
void test_estimate_per_user(void)          { /* same start/end/km=15: u1 total 1650, u2 total 1050 (4 h × 1,50 € + 4,50 €) */ }
void test_estimate_club_zero_any_tariff(void) { /* u2 &club=1 -> 0; u3 without assignment &club=1 -> 0 and status ok */ }
void test_price_tariff_has_no_name(void)   { /* estimate and history JSON: price.tariff has keys rate_day_cents_per_h and tariff_id, no key "name" */ }
void test_switch_applies_from_valid_from(void) {
  /* u1: th_assign_tariff(u1, red, B0 + 2*H); planted done bookings booked_start B0 (std) and B0 + 2*H (red), 1 h each, 0 km:
     history totals 300 and 150 */
}
```

`test_api_billing.c`:

```c
void test_draft_prices_each_user_at_own_tariff(void) {
  /* red tariff (day 150, no caps) assigned to ua at 2 (std stays at 1); add_done(uz, car1, APR10, 2, 0) and add_done(ua, car1, APR11, 2, 0);
     draft groups: Anna total 300, Zora total 600 */
}
void test_repair_clears_no_tariff_blocker(void) {
  /* ub = th_add_user(a, "bo@x.de", …); th_assign_tariff(ub, std, APR10 + DAY); add_done(ub, car1, APR10, 2, 0);
     create_run("2026-04"); expect_blocker("no_tariff", bid);
     PUT /api/admin/user-tariffs/<that id> {"valid_from": 1} -> 200; GET run detail: blockers.no_tariff empty (Review Focus 4) */
}
```

`test_api_notify.c`:

```c
void test_late_reminder_fee_from_user_tariff(void) {
  /* two tariffs (late fee 300 and 900) assigned to u1 and to a second active user u2; active booking of u2 -> mail body says "9,00 €" */
}
```

- [ ] **Step 2: Run**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: all pass. If one fails, fix the owning task's code and re-run.

- [ ] **Step 3: Commit**

```bash
git add server/test/
git commit -m "test: pricing, billing and reminders follow the user's tariff"
```

---

### Task 5: Web — "Tarife" admin page, car page cleanup, estimate without car

**Files:**
- Create: `web/js/views/admin_tariffs.js`
- Modify: `web/js/views/admin_cars.js` (remove `tariffSection`, the "Tarife" button/host and the now-unused imports), `web/js/views/booking_form.js:122` (query without `car=`), `web/js/admin_ui.js:15-19` (nav entry `['#/admin/tarife', 'Tarife']` after Autos), `web/js/app.js` (`'#/admin/tarife': 'admin_tariffs'`), `web/sw.js` (add `/js/views/admin_tariffs.js` to `SHELL`, `CACHE = 'ocs-v19'`)

**Interfaces:**
- Consumes: Task 2 routes; `tariffSummary`, `minutesToHHMM`, `hhmmToMinutes`, `parsePercentToPermille` from `price.js`; `el, adminNav, messageBox, textField, primaryButton` from `admin_ui.js`; `dateTime, toEpoch` from `form_time.js`; `parseEuroToCents, formatEuro` from `money.js`.
- Produces: `export async function render(root)` in `admin_tariffs.js`.

- [ ] **Step 1: Build `admin_tariffs.js`**: `render` loads `GET /api/admin/tariffs` and shows `adminNav('#/admin/tarife')`, `h1 "Tarife"`, an error box, one `card` per tariff and a form "Neuer Tarif" (name field, button "Tarif anlegen" → `POST /api/admin/tariffs`). Per tariff card: `h2` with the name and an inline rename (text field prefilled, button "Umbenennen" → `PUT`), a "Löschen" button (`DELETE`; the 409 message lands in the error box), the version list (`ab <formatDateTime>: <tariffSummary>`, "Löschen" only when `valid_from > now`, `DELETE /api/admin/tariff-versions/<id>`) and the version form moved from `admin_cars.js` as "Neue Version ab", posting to `POST /api/admin/tariffs/<id>/versions`. Prefill: the tariff's newest version, else the newest version of the first tariff in the list, else empty. Any change reloads the page (`root.replaceChildren(); render(root)`).

- [ ] **Step 2: Clean `admin_cars.js`**: remove `tariffSection`, the `tar` button and `tarHost`, and the imports it alone used (`formatEuro`, `formatDateTime`, `dateTime`, `toEpoch`, the `price.js` import, `parseEuroToCents`). Keep `actions` with `edit` and the deactivate button.

- [ ] **Step 3: `booking_form.js`**: the estimate query becomes `` `start=${start}&end=${end}&km=${Number(kmText)}${club.checked ? '&club=1' : ''}` ``; remove `carId` from that line only (it is still used for the booking itself).

- [ ] **Step 4: Router, nav, service worker** as listed in Files.

- [ ] **Step 5: Verify**

Run: `node --test web/test/` → all pass. Run: `node --check web/js/views/admin_tariffs.js web/js/views/admin_cars.js web/js/views/booking_form.js` → no output. Run: `grep -c "admin_tariffs" web/sw.js web/js/app.js web/js/admin_ui.js` → `1`, `1`, `0` (nav uses the hash, not the module name) and `grep -n "ocs-v19" web/sw.js` → one line.

- [ ] **Step 6: Commit**

```bash
git add web/
git commit -m "feat(web): admin Tarife page; tariffs leave the car page"
```

---

### Task 6: Web — tariff on the admin user form and list

**Files:**
- Modify: `web/js/views/admin_users.js`, `web/sw.js` (`CACHE = 'ocs-v20'`)

**Interfaces:**
- Consumes: Task 3 routes; `GET /api/admin/tariffs` for names; `selectField(label, options, value)` from `admin_ui.js`; `dateTime, toEpoch` from `form_time.js`; `formatDateTime` from `time.js`.

- [ ] **Step 1: Load tariffs once** in `render`: `const tariffs = await api('GET', '/api/admin/tariffs')`; `const tariffName = (id) => (tariffs.find((t) => t.id === id) || {}).name || 'keiner'`.

- [ ] **Step 2: Table column "Tarif"** after "Vereinsfahrten": `tariffName(u.tariff_id)`, class `warn` when `u.tariff_id === 0`.

- [ ] **Step 3: Create form**: `selectField('Tarif', tariffs.map((t) => [String(t.id), t.name]), tariffs[0] ? String(tariffs[0].id) : '')` shown only for `!u`; body gets `tariff_id: Number(tariffSel.input.value)`. With no tariffs at all the create form shows the hint „Bitte zuerst einen Tarif anlegen.“ and disables the submit button.

- [ ] **Step 4: Edit form `tariffSection(u)`** (sibling of `mandateSection`): `h3 "Tarif"`, loads `GET /api/admin/users/<id>/tariffs`; empty → „Tarif: keiner“. Rows newest first: „ab <formatDateTime(valid_from)>: <tariffName(tariff_id)>“, the row in force (largest `valid_from <= now`) suffixed „(gilt)“. Newest row only: "Bearbeiten" (reveals select + `dateTime(valid_from)` + "Speichern" → `PUT /api/admin/user-tariffs/<id>` with both fields) and "Löschen" (`window.confirm` „Tarifzuordnung löschen?“ → `DELETE`). Below: form "Tarif wechseln": select (preselect the tariff in force) + `dateTime(Math.ceil((now + 900) / 900) * 900)` + button → `POST /api/admin/users/<id>/tariffs`. Errors go to the form's `fErr`; success reloads with „Gespeichert.“.

- [ ] **Step 5: Verify**

Run: `node --test web/test/` → all pass. Run: `node --check web/js/views/admin_users.js` → no output. `grep -n "ocs-v20" web/sw.js` → one line.

- [ ] **Step 6: Commit**

```bash
git add web/
git commit -m "feat(web): tariff select and assignment history on the admin user form"
```

---

### Task 7: Spec and docs

**Files:**
- Modify: `docs/spec.md` §4 (user, tariff), §6, §9 step 2, §10 late-return reminder, §13 admin routes, §15 (Phase 4 note + new phase entry); `docs/superpowers/specs/2026-11-02-phase4-pricing-design.md` (one-line note at the top pointing to the new design)

- [ ] **Step 1: §4** — replace the `### tariff (per car, versioned)` block with three blocks: `### tariff` (id, name unique, created_at; `data/tariffs.jsonl`), `### tariff_version` (fields as today, `tariff_id`; `data/tariff_versions.jsonl`), `### user_tariff` (id, user_id, tariff_id, valid_from, created_at, created_by; append-only, repair rule for the newest row; `data/user_tariffs.jsonl`). Replace "Pricing uses the tariff valid at the booking's booked start." with: "Pricing uses the booking user's assignment in force at `booked_start` and that tariff's version in force at `booked_start`; either missing means no tariff." Add to `### user`: "Tariff: see user_tariff; member endpoints never return tariff data."

- [ ] **Step 2: §6, §9, §10** — reword the three "tariff at `booked_start`" mentions (lines ~104, ~348, ~514) to "the user's tariff at `booked_start`"; `no_tariff` blocker text: "included non-cancelled non-club bookings whose user has no tariff at `booked_start`".

- [ ] **Step 3: §13** — in the admin block replace `/tariffs` with the ten routes of Tasks 2–3 (one line each, as in the design's API section) and note `POST /api/admin/users` requires `tariff_id`, `GET /api/admin/users` includes `tariff_id`, export includes `tariffs`. Member block: `GET /api/price-estimate?start&end&km[&club=1]`.

- [ ] **Step 4: §15** — append after Phase 7:

```
### Phase 8: User tariffs — done (merged <date>)
Design: `docs/superpowers/specs/2026-11-09-user-tariffs-design.md`.
Plan: `docs/superpowers/plans/2026-11-09-user-tariffs.md`.
- Tariffs are named global price lists with append-only versions; users carry
  dated assignments; pricing follows the user's tariff at `booked_start`.
- Forward-dated switch, repair path on the newest assignment (blocked by
  invoiced bookings), "Tarife" admin page, tariff select on user creation.
Done when: two users on different tariffs get different prices for the same
trip, a switch re-prices only bookings starting after it, club trips stay 0 €.
```

and under Phase 4 add "(tariffs moved from cars to users in Phase 8)".

- [ ] **Step 5: Verify** — `grep -n "per car\|car_id" docs/spec.md` shows no tariff-related hit; `grep -n "cars/{id}/tariffs\|cars/\*/tariffs" docs/ -r` → none.

- [ ] **Step 6: Commit**

```bash
git add docs/
git commit -m "docs(spec): tariffs per user, Phase 8 entry"
```

---

## Self-review notes

- Spec coverage: data model (T1), lookup (T1), tariff rules (T2), assignment rules incl. repair and no-assignment state (T3), member estimate without `car` and no-name (T1, T4), club trips (T4), billing blocker and per-user billing (T4), reminder (T4), admin UI (T5, T6), storage files (T1), spec update (T7), GDPR export (T3). Fixtures rewritten (T1). Not deployed → no migration task.
- Names used consistently: `tariff_t`, `tariff_version_t`, `user_tariff_t`, `db_tariff_at(db, user_id, ts)`, `db_version_at`, `db_user_tariff_at`, `db_user_tariff_newest`, `th_add_tariff`, `th_add_version`, `th_assign_tariff`, route paths as in the design.
