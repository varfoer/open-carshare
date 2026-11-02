# Phase 5: SEPA Mandates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Users give, replace and revoke a SEPA direct debit mandate online; bookings need a current mandate; the admin sees who lacks one and maintains the creditor data.

**Architecture:** Pure SEPA helpers (IBAN/BIC/creditor ID checks, masking, mandate reference, mandate text and confirmation mail body) live in `core/sepa.c`. Mandates are a new append-style db table (`mandates.jsonl`): one row per mandate with its evidence; a user's current mandate is the row with `status == active`. `http/api_mandate.c` serves the user and admin endpoints and the shared revoke routine; `api_booking.c` gates create/extend/start on a current mandate.

**Tech Stack:** unchanged: C11, CMake, Mongoose 7.14, cJSON, Unity, vanilla JS (ES2020), `node --test`.

**Spec:** `docs/spec.md` §4 (user, settings), §9 SEPA, §13, §15 Phase 5; product-owner decisions below (2026-11-02) are binding and go into the spec in Task 9.

**Branch:** `phase5-sepa-mandates`.

**Out of scope:** `pain.008` export, `sepa lead days`, invoices, pre-notification, transliteration of holder names for XML (all Phase 6).

## Product-owner decisions (2026-11-02)

- Mandates are a separate table with history; a new mandate ends the previous one (`replaced`), old rows are kept as evidence.
- User and admin can revoke a mandate. Revocation deactivates the account (sessions ended) and cancels all `reserved` bookings of the user with cancel fee 0; an `active` booking stays and is billed normally.
- Without a current mandate a user cannot create, change (`PUT`) or start a booking via the user API: 403 `no_mandate`. Club trips are not exempt. Admin API bookings are exempt.
- After giving a mandate the user gets a confirmation mail (text, reference, masked IBAN). Mail failure does not undo the mandate.
- Mandate text is built into the server, versioned by a constant; creditor name and creditor ID come from settings. Users cannot give a mandate until both are set.

## Global Constraints

- All Phase 1–4 Global Constraints still hold (`int64_t` UTC seconds, no float, German UI, English code, fixed `char[]` reject-not-truncate, `-Wall -Wextra -Werror`, ASan/UBSan tests, db copy-before-modify, every changed web file in `web/sw.js` `SHELL` and `CACHE` bumped once per task that changes web files).
- Build/test from repo root: `cmake --build build && ctest --test-dir build --output-on-failure`; web: `node --test web/test/`.
- IBAN, BIC, creditor ID are stored normalized: spaces removed, `a-z` uppercased. Any other character than `[A-Za-z0-9 ]` is invalid.
- IBAN: country in the SEPA length table (Task 1), exact length, check digits via mod-97 == 1.
- Full IBANs never leave the server in any API response or mail; only `iban_masked` (`"DE89 **** 3000"`: first 4, `" **** "`, last 4).
- Mandate reference: `"OCS-%06lld"` of the mandate id (`OCS-000012`), derived, not stored.
- Account holder: non-empty, at most 70 UTF-8 code points (SEPA `Nm` limit), stored in `char[256]`.
- `MANDATE_TEXT_VERSION` = 1. Any change of the text bumps it.
- Nothing is deployed: no backward compatibility with older data files or API shapes needed. A missing `mandates.jsonl` (fresh data dir) = no mandates.

## Review Focus

1. **IBAN typed as printed** (`"de89 3704 0044 0532 0130 00"`) is accepted and stored as `DE89370400440532013000`. Test: Task 1 `test_normalize_iban_spaces_lowercase`, Task 4 `test_post_mandate_normalizes`.
2. **Double submit** of the same holder/IBAN/BIC creates no second mandate and no new reference. Test: Task 4 `test_post_same_data_is_idempotent`.
3. **Last active admin revokes own mandate** would lock everyone out: 409 `last_admin`, nothing changed. Test: Task 5 `test_revoke_last_admin_409`.
4. **Creditor data or text changes after the user opened the form:** stale `text_version` → 409 `text_changed`; an existing mandate keeps its snapshot of creditor name/ID. Test: Task 4 `test_post_stale_text_version_409`, `test_mandate_keeps_creditor_snapshot`.
5. **Revoke inside the cancel deadline / during a trip:** reserved bookings cancelled with fee 0 even when `cancel_fee_cents > 0`; the active booking stays active. Test: Task 5 `test_revoke_cancels_reserved_keeps_active`.

---

## File Structure

```
server/src/core/sepa.{h,c}            NEW: IBAN/BIC/creditor ID, mask, ref, mandate text, mail body
server/src/core/model.h               mandate_t, settings_t creditor fields
server/src/core/codec.{h,c}           mandate codec, settings string fields
server/src/store/db.{h,c}             mandates table
server/src/http/api_mandate.c         NEW: /api/me/mandate, admin mandate endpoints, revoke routine
server/src/http/api_admin.c           settings fields, users list "mandate"
server/src/http/api_booking.c         no_mandate gate
server/src/http/api.c, api_internal.h routes, declarations, error helper
server/test/test_sepa.c, test_api_mandate.c  NEW
server/test/helpers.{h,c}             th_add_user gives a mandate; th_add_user_bare; th_set_creditor
web/js/sepa.js, web/test/sepa.test.mjs      NEW
web/js/views/account.js, admin_users.js, admin_settings.js, web/sw.js
docs/spec.md, docs/backlog.md, deploy/vps/README.md
```

---

### Task 1: Pure SEPA helpers

**Files:**
- Create: `server/src/core/sepa.{h,c}`, `server/test/test_sepa.c`
- Modify: `server/CMakeLists.txt` (`ocs_add_test(test_sepa)`; sources are globbed)

**Interfaces:**
- Produces (`core/sepa.h`):
  ```c
  #define MANDATE_TEXT_VERSION 1
  #define SEPA_HOLDER_MAX_CHARS 70
  int  sepa_normalize(const char *in, char *out, size_t cap); /* 0 ok; -1 bad char or does not fit */
  bool iban_valid(const char *iban);         /* normalized input */
  bool bic_valid(const char *bic);           /* normalized: 4 letters, 2 letters, 2 alnum, optional 3 alnum */
  bool creditor_id_valid(const char *ci);    /* normalized */
  void iban_mask(const char *iban, char *out, size_t cap);   /* "DE89 **** 3000"; "" if strlen < 8 */
  void mandate_ref(int64_t id, char *out, size_t cap);       /* "OCS-000012" */
  int  mandate_text(char *out, size_t cap, const char *creditor_name, const char *creditor_id); /* length or -1 */
  ```
- SEPA IBAN lengths: AD24 AT20 BE16 BG22 CH21 CY28 CZ24 DE22 DK18 EE20 ES24 FI18 FR27 GB22 GI23 GR27 HR21 HU28 IE22 IS26 IT27 LI21 LT20 LU20 LV21 MC27 MT31 NL18 NO15 PL28 PT25 RO24 SE24 SI19 SK24 SM27 VA22.
- Creditor ID: 2 letters, 2 digits, 3 alnum business code (ignored in check), 1..28 alnum national part; mod-97 over `national + country + check digits` == 1; length 18 if country `DE`, else 8..35.
- Mod-97: letters A=10..Z=35, remainder computed digit by digit (no bignum).
- Mandate text (exact copy, `{N}` = creditor name, `{C}` = creditor ID):
  ```
  SEPA-Lastschriftmandat

  Zahlungsempfänger: {N}
  Gläubiger-Identifikationsnummer: {C}
  Mandatsreferenz: wird nach Erteilung des Mandats mitgeteilt

  Ich ermächtige {N}, Zahlungen von meinem Konto mittels Lastschrift einzuziehen. Zugleich weise ich mein Kreditinstitut an, die von {N} auf mein Konto gezogenen Lastschriften einzulösen.

  Hinweis: Ich kann innerhalb von acht Wochen, beginnend mit dem Belastungsdatum, die Erstattung des belasteten Betrages verlangen. Es gelten dabei die mit meinem Kreditinstitut vereinbarten Bedingungen.

  Das Mandat gilt für wiederkehrende Lastschriften.
  ```
- [ ] **Step 1: Write the failing tests** (`test_sepa.c`):
  - `test_normalize_iban_spaces_lowercase`: `"de89 3704 0044 0532 0130 00"` → `"DE89370400440532013000"`; `"DE89-3704"` → -1; 40 chars into `char[35]` → -1.
  - `test_iban_valid_vectors`: true for `DE89370400440532013000`, `GB82WEST12345698765432`, `NL91ABNA0417164300`, `AT611904300234573201`; false for `DE89370400440532013001` (check), `DE8937040044053201300` (length), `US64SVBKUS6S3300958879` (not SEPA), `""`.
  - `test_bic_valid`: true `COBADEFFXXX`, `COBADEFF`; false `COBADEF`, `COBADEFFXX`, `1OBADEFF`, `""`.
  - `test_creditor_id_valid`: true `DE98ZZZ09999999999`; false `DE98ZZZ09999999998`, `DE98ZZZ0999999999` (DE length), `""`.
  - `test_iban_mask`: `DE89370400440532013000` → `"DE89 **** 3000"`.
  - `test_mandate_ref`: 12 → `"OCS-000012"`, 1234567 → `"OCS-1234567"`.
  - `test_mandate_text_fills_creditor`: contains `"Ich ermächtige Carsharing Testverein e.V., Zahlungen"` and `"Gläubiger-Identifikationsnummer: DE98ZZZ09999999999"`; cap 10 → -1.
- [ ] **Step 2: Run** `cmake --build build && ctest --test-dir build -R test_sepa --output-on-failure` — Expected: compile error.
- [ ] **Step 3: Implement** `core/sepa.{h,c}` per Interfaces.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Commit** `feat(core): SEPA validation, mandate ref and text`.

---

### Task 2: Mandate model, codec, db table; settings fields

**Files:**
- Modify: `server/src/core/model.h`, `core/codec.{h,c}`, `store/db.{h,c}`, `core/sepa.c` (`mandate_mail_body`)
- Test: `server/test/test_codec.c`, `test_db.c`, `test_sepa.c`

**Interfaces:**
- Consumes: Task 1 validators.
- Produces (`model.h`):
  ```c
  typedef enum { MANDATE_ACTIVE = 0, MANDATE_REPLACED, MANDATE_REVOKED } mandate_status_t;
  typedef struct {
    int64_t id, user_id, created_at;      /* created_at = mandate_date */
    int64_t ended_at, ended_by;           /* 0 while active; ended_by = acting user id */
    mandate_status_t status;
    char holder[256], iban[35], bic[12];  /* bic "" = none */
    char client_ip[64]; int text_version;
    char creditor_name[96], creditor_id[36];  /* snapshot at creation */
  } mandate_t;
  ```
  `settings_t` gains `char assoc_name[96], assoc_street[96], assoc_zip[16], assoc_city[64], creditor_id[36], creditor_iban[35], creditor_bic[12];` (appended after the existing fields, so `{15, 60, 500}` initializers still compile).
- Produces (`codec.h`): `cJSON *mandate_to_json(const mandate_t *)`, `int mandate_from_json(const cJSON *, mandate_t *)` — all keys required; status names `"active"|"replaced"|"revoked"`; -1 if `iban` not `iban_valid`, `bic` not `""`/`bic_valid`. Holder written with `put_cstr` after a length check (as `damage_report_to_json` does for `text`). Settings: string keys handled like the existing ones (missing on load → `""`, the fresh-data-dir default), always written.
- Produces (`db.h`):
  ```c
  mandate_t *mandates; size_t n_mandates, cap_mandates;   /* in db_t */
  mandate_t *db_mandate(db_t *db, int64_t id);
  const mandate_t *db_mandate_current(const db_t *db, int64_t user_id); /* status ACTIVE, largest id; NULL if none */
  int db_put_mandate(db_t *db, mandate_t *m);   /* id==0: assign max+1 and insert; else replace */
  ```
  File `mandates.jsonl`; same pattern as tariffs (`DEFINE_LOADER_X`, `DEFINE_PUT_ENTITY`).
- Produces (`core/sepa.h`): `int mandate_mail_body(char *out, size_t cap, const char *user_name, const mandate_t *m); /* length or -1 */`
- Mail body (exact copy, `{R}` ref, `{H}` holder, `{M}` masked IBAN, text with the mandate's snapshot `creditor_name`/`creditor_id`):
  ```
  Hallo {user_name},

  du hast online ein SEPA-Lastschriftmandat erteilt.

  Mandatsreferenz: {R}
  Kontoinhaber: {H}
  IBAN: {M}

  {mandate text}
  ```


- [ ] **Step 1: Write the failing tests:**
  - `test_codec.c` `test_mandate_roundtrip`: all fields survive; `test_mandate_rejects_bad_iban` (`DE89370400440532013001` → -1); `test_settings_strings_default_empty` (`{}` → `assoc_name[0] == 0`, `start_tolerance_min == 15`); `test_settings_strings_roundtrip`.
  - `test_db.c` `test_mandate_put_and_current`: insert two for user 7 (first REPLACED, second ACTIVE) → current is the second; user 8 → NULL; reload from disk → same; `test_db_loads_without_mandates_file`.
  - `test_sepa.c` `test_mandate_mail_body`: contains `"Mandatsreferenz: OCS-000003"`, `"IBAN: DE89 **** 3000"`, not `"DE89370400440532013000"`.
- [ ] **Step 2: Run** `ctest --test-dir build -R 'test_codec|test_db|test_sepa' --output-on-failure` — Expected: compile errors.
- [ ] **Step 3: Implement** model, codec, db, `mandate_mail_body`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Commit** `feat(store): mandates table, creditor settings`.

---

### Task 3: Admin settings for association and creditor

**Files:**
- Modify: `server/src/http/api_admin.c` (`settings_response`, `h_admin_settings_put`), `web/js/views/admin_settings.js`, `web/sw.js`
- Test: `server/test/test_api_admin.c`

**Interfaces:**
- `PUT /api/admin/settings`: all keys required: the three integers plus strings `assoc_name`, `assoc_street`, `assoc_zip`, `assoc_city`, `creditor_id`, `creditor_iban`, `creditor_bic` (missing → 400 `invalid`). Strings must fit the field; `creditor_id`/`creditor_iban`/`creditor_bic` normalized with `sepa_normalize`, then `""` or valid, else 400 `invalid_creditor_id` „Ungültige Gläubiger-ID.“ / `invalid_iban` „Die IBAN ist ungültig.“ / `invalid_bic` „Die BIC ist ungültig.“
- `GET`/`PUT` response: all settings keys incl. the seven strings (creditor IBAN in full: admin-only, own account of the Verein).
- UI: second card „Verein und Lastschrift“ in the same form: Vereinsname, Straße, PLZ, Ort, Gläubiger-ID, IBAN des Vereins, BIC des Vereins (optional). One „Speichern“ sends all keys.

- [ ] **Step 1: Write the failing tests** (`test_api_admin.c`):
  - `test_settings_creditor_roundtrip`: PUT with `creditor_id: "de98 zzz0 9999 9999 99"` → 200, response `creditor_id == "DE98ZZZ09999999999"`.
  - `test_settings_creditor_invalid_400`: `creditor_id "DE98ZZZ09999999998"` → `invalid_creditor_id`; `creditor_iban "DE89370400440532013001"` → `invalid_iban`; `creditor_bic "XX"` → `invalid_bic`; settings unchanged.
  - `test_settings_put_missing_string_400`: PUT with only the three ints → 400 `invalid`. Update `test_settings_roundtrip_and_negative_400` to send all keys.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_admin --output-on-failure` — Expected: FAIL.
- [ ] **Step 3: Implement** server and UI; bump `CACHE`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` and `node --test web/test/` — all pass.
- [ ] **Step 5: Commit** `feat: creditor and association settings`.

---

### Task 4: Give and replace a mandate (user API)

**Files:**
- Create: `server/src/http/api_mandate.c`, `server/test/test_api_mandate.c`
- Modify: `server/src/http/api.c` (routes), `api_internal.h`, `server/test/helpers.{h,c}`, `server/CMakeLists.txt` (`ocs_add_test(test_api_mandate)`)

**Interfaces:**
- Consumes: Task 1, Task 2 (`db_mandate_current`, `db_put_mandate`), `api_send_mail`, `req->client_ip`.
- Produces (`api_internal.h`):
  ```c
  /* API view of a mandate: id, ref, status, holder, iban_masked, bic, created_at, ended_at,
   * ended_by, client_ip, text_version, creditor_name, creditor_id. Never the full IBAN. NULL on OOM. */
  cJSON *mandate_api_json(const mandate_t *m);
  void h_me_mandate_get(app_t *, const api_req_t *, const api_ctx_t *, api_res_t *);
  void h_me_mandate_post(app_t *, const api_req_t *, const api_ctx_t *, api_res_t *);
  ```
- Produces (`helpers.h`): `th_add_user` now also inserts an ACTIVE mandate (holder `"Test User"`, IBAN `DE89370400440532013000`, `text_version` 1); `int64_t th_add_user_bare(app_t *, const char *email, const char *pw, role_t)` without mandate; `void th_set_creditor(app_t *)` sets `assoc_name "Carsharing Testverein e.V."`, `creditor_id "DE98ZZZ09999999999"`.
- Routes: `GET /api/me/mandate`, `POST /api/me/mandate` (MIN_USER).
- `GET` → 200 `{"mandate": <api json>|null, "ready": bool, "text": str|null, "text_version": 1}`; `ready` = `assoc_name` and `creditor_id` non-empty; `text` = `mandate_text(assoc_name, creditor_id)` when ready, else null.
- `POST` body `{"account_holder", "iban", "bic"?, "text_version", "accept": true}`, checks in this order:
  1. not ready → 409 `no_creditor` „Lastschriftmandate sind noch nicht eingerichtet. Bitte wende dich an den Verein.“
  2. missing/typed wrong, holder empty after trim or > 70 code points, `accept` not `true` → 400 `invalid`
  3. IBAN → 400 `invalid_iban`; BIC (if present and non-empty) → 400 `invalid_bic`
  4. `text_version != MANDATE_TEXT_VERSION` → 409 `text_changed` „Der Mandatstext wurde geändert. Bitte lies ihn erneut.“
  5. current mandate with identical holder, iban, bic → 200 `{"mandate", "mail_sent": false}`, nothing written.
  6. Insert new (ACTIVE, `created_at` now, `client_ip`, `text_version`, creditor snapshot); then set the old current (if any) to REPLACED, `ended_at` now, `ended_by` user. Insert first so a failure in step two never leaves the user without a mandate. Mail subject „SEPA-Lastschriftmandat erteilt“, body `mandate_mail_body`. → 201 `{"mandate", "mail_sent": bool}`.

- [ ] **Step 1: Write the failing tests** (`test_api_mandate.c`, user via `th_add_user_bare`, `th_set_creditor` unless stated):
  - `test_get_not_ready`: no creditor → `ready false`, `text null`, `mandate null`.
  - `test_post_not_ready_409`: → `no_creditor`.
  - `test_post_mandate_normalizes`: IBAN `"de89 3704 0044 0532 0130 00"` → 201; stored `iban == "DE89370400440532013000"`; response `ref == "OCS-000001"`, `iban_masked == "DE89 **** 3000"`, `client_ip == "1.2.3.4"`; body does not contain `"0532013000"`; one mail to the user containing `"OCS-000001"`.
  - `test_post_rejects_bad_input`: bad IBAN → `invalid_iban`; BIC `"XX"` → `invalid_bic`; `accept false` → `invalid`; 71-char holder → `invalid`; no mandate stored.
  - `test_post_stale_text_version_409`: `text_version 0` → `text_changed`.
  - `test_post_same_data_is_idempotent`: second identical POST → 200, `n_mandates == 1`, `mail_sent false`.
  - `test_post_new_iban_replaces`: second POST with `GB82WEST12345698765432` → 201; first row REPLACED with `ended_at == now`; current is the second, ref `OCS-000002`.
  - `test_mandate_keeps_creditor_snapshot`: give mandate, change `assoc_name`, GET → mandate `creditor_name` is the old name.
  - `test_post_mail_failure_still_201`: `th_mail_fail(1)` → 201, `mail_sent false`, mandate stored.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_mandate --output-on-failure` — Expected: compile error.
- [ ] **Step 3: Implement** handlers, routes, helpers.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass (existing tests keep working because `th_add_user` gives a mandate).
- [ ] **Step 5: Commit** `feat(api): give and replace SEPA mandate`.

---

### Task 5: Revoke (user and admin), admin mandate views

**Files:**
- Modify: `server/src/http/api_mandate.c`, `api.c`, `api_internal.h`, `api_admin.c` (`h_admin_users_get`)
- Test: `server/test/test_api_mandate.c`, `test_api_admin.c`

**Interfaces:**
- Produces (`api_internal.h`):
  ```c
  /* Ends the current mandate of user_id as REVOKED (ended_by actor_id), cancels every RESERVED
   * booking of the user (cancel_fee_cents 0, cancelled_at/modified_at now, modified_by actor_id),
   * sets the user inactive and deletes the user's sessions, in this order.
   * 0 ok, -1 storage error (steps done so far stay), -2 no current mandate. */
  int api_mandate_revoke(app_t *a, int64_t user_id, int64_t actor_id, int64_t now);
  void h_me_mandate_delete(app_t *, const api_req_t *, const api_ctx_t *, api_res_t *);
  void h_admin_user_mandates_get(app_t *, const api_req_t *, const api_ctx_t *, api_res_t *);
  void h_admin_user_mandate_revoke(app_t *, const api_req_t *, const api_ctx_t *, api_res_t *);
  ```
- `DELETE /api/me/mandate` (MIN_USER) body `{"password"}`: wrong/missing password → 400 `bad_credentials`; no current mandate → 404 `not_found`; actor is admin and no other active admin → 409 `last_admin` „Du bist der letzte aktive Admin. Bitte ernenne zuerst einen weiteren Admin.“; else revoke, clear the session cookie (`ocs_session=; ...; Max-Age=0`, as `h_logout`), 200 `{"ok":true}`.
- `GET /api/admin/users/*/mandates` (MIN_ADMIN): 404 unknown user; 200 array of `mandate_api_json`, newest first.
- `POST /api/admin/users/*/mandate/revoke` (MIN_ADMIN): id == actor → 400 `self` (existing message); unknown user or no current mandate → 404; else revoke → 200 `{"ok":true}`.
- `GET /api/admin/users`: each user object gains `"mandate": {"ref", "created_at", "iban_masked"}` or `null` (current mandate).

- [ ] **Step 1: Write the failing tests:**
  - `test_api_mandate.c`:
    - `test_revoke_cancels_reserved_keeps_active`: `cancel_fee_cents 500`, `cancel_deadline_min 1440`; user has one ACTIVE and two RESERVED bookings (one starting in 10 min) → DELETE 200; both RESERVED → CANCELLED with `cancel_fee_cents 0`; ACTIVE unchanged; user `active false`; no sessions of user; mandate REVOKED with `ended_by == user`; `Set-Cookie` contains `Max-Age=0`.
    - `test_revoke_wrong_password_400`: nothing changed.
    - `test_revoke_without_mandate_404`.
    - `test_revoke_last_admin_409`: only admin revokes own → 409, admin still active; with a second active admin → 200.
    - `test_admin_revoke_other_user`: 200, same effects as user revoke, `ended_by == admin id`.
    - `test_admin_revoke_self_400`.
    - `test_admin_mandates_history_newest_first`: two mandates → array ids `[2, 1]`, no key `iban`.
    - `test_reactivated_user_has_no_mandate`: after revoke, admin sets `active true` → `GET /api/me/mandate` `mandate null`.
  - `test_api_admin.c` `test_users_list_mandate_field`: user with mandate → `mandate.ref` `"OCS-…"`, `th_add_user_bare` user → `mandate` null.
- [ ] **Step 2: Run** `ctest --test-dir build -R 'test_api_mandate|test_api_admin' --output-on-failure` — Expected: FAIL.
- [ ] **Step 3: Implement**; the last-admin check counts users with `role ADMIN && active && id != actor`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Commit** `feat(api): revoke SEPA mandate, admin mandate views`.

---

### Task 6: Booking gate

**Files:**
- Modify: `server/src/http/api_booking.c`
- Test: `server/test/test_api_booking.c`

**Interfaces:**
- Consumes: `db_mandate_current`.
- `h_booking_post` (before body parsing), `h_booking_put` and `h_booking_start` (after `own_booking`, so foreign ids stay 404): no current mandate for `ctx->user->id` → 403 `no_mandate` „Bitte erteile zuerst unter „Konto“ ein SEPA-Lastschriftmandat.“ Cancel, return, damage stay ungated. Admin endpoints unchanged.

- [ ] **Step 1: Write the failing tests** (`test_api_booking.c`, user via `th_add_user_bare`):
  - `test_create_without_mandate_403`: POST → 403 `no_mandate`; also with `club_trip true` and `may_club_trip`.
  - `test_put_and_start_without_mandate_403`: booking inserted via `db_put_booking` → PUT extend 403, start 403; DELETE (cancel) 200.
  - `test_admin_creates_booking_for_user_without_mandate`: `POST /api/admin/bookings` → 201.
  - `test_foreign_booking_still_404`: other user's booking id → PUT 404.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_booking --output-on-failure` — Expected: FAIL.
- [ ] **Step 3: Implement** a static helper in `api_booking.c`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` — all pass.
- [ ] **Step 5: Commit** `feat(api): bookings need a SEPA mandate`.

---

### Task 7: Web — mandate on the account page

**Files:**
- Create: `web/js/sepa.js`, `web/test/sepa.test.mjs`
- Modify: `web/js/views/account.js`, `web/sw.js` (`SHELL` += `/js/sepa.js`, bump `CACHE`)

**Interfaces:**
- Produces (`sepa.js`): `normalizeIban(s) -> string` (drop spaces, uppercase), `ibanValid(s) -> boolean` (normalized `/^[A-Z]{2}\d{2}[A-Z0-9]{11,30}$/` and mod-97 == 1, chunked, no BigInt; server stays authoritative for the country table), `formatIban(s) -> string` (groups of 4 separated by one space).
- Account page, card „SEPA-Lastschriftmandat“ between profile and password:
  - `ready false`: hint „Lastschriftmandate sind noch nicht eingerichtet.“
  - Mandate present: Mandatsreferenz, Kontoinhaber, IBAN (`iban_masked`), erteilt am (Berlin date), buttons „Bankverbindung ändern“ (opens the form) and „Mandat widerrufen“.
  - No mandate: hint „Ohne Mandat kannst du keine Fahrzeuge buchen.“ and the form.
  - Form: Kontoinhaber (prefilled `user.name`), IBAN (formatted on blur), BIC (optional), mandate `text` in a `<pre class="mandate-text">`, checkbox „Ich erteile dieses SEPA-Lastschriftmandat.“ (required), button „Mandat erteilen“. Client check with `ibanValid` before sending: „Die IBAN ist ungültig.“ Sends `text_version` from the GET. Success: re-render card, message „Mandat erteilt. Die Bestätigung kommt per E-Mail.“ (or „…, die E-Mail konnte aber nicht gesendet werden.“ when `mail_sent false`).
  - Revoke: inline confirm area with warning „Dein Konto wird deaktiviert und deine offenen Reservierungen werden storniert. Du wirst abgemeldet.“, password field, button „Endgültig widerrufen“. On 200 → `refreshUser()` and navigate to `#/login`.

- [ ] **Step 1: Write the failing tests** (`sepa.test.mjs`): `normalizeIban(' de89 3704 ')  === 'DE893704'`; `ibanValid` true for `DE89 3704 0044 0532 0130 00`, `GB82WEST12345698765432`; false for `DE89370400440532013001`, `'DE89'`, `''`; `formatIban('DE89370400440532013000') === 'DE89 3704 0044 0532 0130 00'`.
- [ ] **Step 2: Run** `node --test web/test/` — Expected: FAIL (module missing).
- [ ] **Step 3: Implement** `sepa.js` and the account card.
- [ ] **Step 4: Run** `node --test web/test/` — all pass; manual check in the browser: give, change, revoke (use the `run` skill).
- [ ] **Step 5: Commit** `feat(web): SEPA mandate on account page`.

---

### Task 8: Web — admin users mandate status

**Files:**
- Modify: `web/js/views/admin_users.js`, `web/sw.js` (bump `CACHE`)

**Interfaces:**
- Consumes: `GET /api/admin/users` `mandate`, `GET /api/admin/users/{id}/mandates`, `POST /api/admin/users/{id}/mandate/revoke`.
- Table column „Mandat“: `mandate.ref` or „fehlt“ (class `warn`).
- Edit form: section „Lastschriftmandate“ listing history (Referenz, Status `aktiv|ersetzt|widerrufen`, Kontoinhaber, IBAN masked, erteilt am, beendet am, IP, Textversion); for an active mandate a button „Mandat widerrufen“ with `window.confirm('Mandat widerrufen? Das Konto wird deaktiviert und offene Reservierungen werden storniert.')`, then `reload('Mandat widerrufen.')`. Not shown for the admin's own row.

- [ ] **Step 1: Run** `node --test web/test/` — baseline passes (no pure logic added; this task is DOM only).
- [ ] **Step 2: Implement** column, history, revoke button.
- [ ] **Step 3: Manual check** in the browser: column shows „fehlt“ for a user without mandate, revoke deactivates the user.
- [ ] **Step 4: Commit** `feat(web): admin sees and revokes mandates`.

---

### Task 9: Spec, backlog, deployment notes

**Files:**
- Modify: `docs/spec.md`, `docs/backlog.md`, `deploy/vps/README.md`

- [ ] **Step 1: Update `docs/spec.md`:** §4 user: drop `account_holder, iban, bic, mandate_ref, mandate_date`; add `### mandate` (fields of `mandate_t`, ref derived `OCS-%06d` from id, current = `active`); §4 settings: list the seven string keys; §9 SEPA: product-owner decisions above (history, revocation effects, booking gate incl. club trips, confirmation mail, built-in versioned text, IBAN restricted to SEPA countries, full IBAN never in API/mail); §13: the five new routes.
- [ ] **Step 2: Update `deploy/vps/README.md`:** first setup: the admin enters Verein data and Gläubiger-ID in „Einstellungen“; every member (admins included) gives a mandate under „Konto“ before booking.
- [ ] **Step 3: Update `docs/backlog.md`:** add `[P6]` items: holder names need SEPA character-set transliteration for `pain.008`; non-DE IBANs may need the BIC in pain.008 for non-EEA countries (CH, GB, …).
- [ ] **Step 4: Run** `cmake --build build && ctest --test-dir build --output-on-failure && node --test web/test/` — all pass.
- [ ] **Step 5: Commit** `docs: phase 5 SEPA mandates in spec`.

After merge: mark Phase 5 done in spec §15 (separate commit, as for earlier phases).
