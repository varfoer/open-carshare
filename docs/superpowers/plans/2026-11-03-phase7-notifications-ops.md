# Phase 7: Notifications and Operations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every §10 mail goes out (booking created/modified/cancelled, start reminder, late-return reminder, billing run done), the nightly age-encrypted backup is mailed and restores, and the admin can export a user's data and deactivate them without leaving dangling reservations.

**Architecture:** Mail texts are pure builders in a new `core/notify.c` (golden-tested). A new `http/api_notify.c` is the only glue: it looks up user/car/tariff, skips inactive users, sends, and never changes the HTTP outcome of the request that triggered it. Reminders run in the existing 5-min expire timer and are made once-only by two persisted booking flags. The backup is a bash script plus systemd timer (`tar | age | curl smtp`), round-trip tested from `ctest`; the server only learns to accept the `backup_*` config keys and gains a read-only `check-data` subcommand used to verify a restore.

**Tech Stack:** unchanged: C11, CMake, Mongoose, cJSON, Unity, vanilla JS, `node --test`. New runtime/dev tool: `age` (Debian package `age`, provides `age` and `age-keygen`), bash, GNU tar, coreutils `base64`, curl CLI.

**Spec:** `docs/spec.md` §4 (booking, settings), §10, §12, §13, §15 Phase 7. Decisions below are binding and go into the spec in Task 10.

**Branch:** `phase7-notifications-ops`.

**Out of scope:** per-user mail opt-out; mails on cancellations caused by mandate revocation or account deactivation; automatic purge after 10 years; embedding damage photos in the export; mails to the next booker of an overdue car.

## Product-owner decisions (2026-11-03)

1. **Backup is a shell script + systemd timer** using the `age` CLI and curl to the configured SMTP relay. No crypto in C.
2. **GDPR export carries the masked IBAN** (`iban_masked`); the "full IBANs never leave the server" rule stays without exception.
3. **Invoice retention = guarantee only:** nothing deletes invoices, invoice lines, invoice PDFs, mandates or invoiced bookings; deactivated users keep their records. Documented and tested; no purge job.
4. **"Billing run done" mail goes out after each send call** that reached the mail loop, to all active admins.

## Builder calls (not asked; change in review if wrong)

1. **Recipient** of every booking mail is the booking's user (not the actor). An admin creating/editing/cancelling a member's booking mails the member. Users with `active == false` get no booking or reminder mail.
2. **Triggers:** created = user `POST /api/bookings` and admin `POST /api/admin/bookings`. Modified = user or admin `PUT` where `booked_start`, `booked_end`, `car_id` or `club_trip` changed (admin edits of actual times/km only send nothing). Cancelled = user or admin `DELETE`. Start, return, expiry, revoke- and deactivation-cancellations send nothing.
3. **Mail failure never changes the response** of the booking request (same rule as the mandate mail); the mailer logs it.
4. **Start reminder:** setting `reminder_before_min` (integer 0..1440, default 60, 0 = off; required in the admin settings PUT like every key). Sent once for a `reserved` booking when `booked_start - reminder_before_min <= now < booked_start`, unless the booking was created inside that window (`created_at >= booked_start - reminder_before_min`; the created mail already told them).
5. **Late-return reminder:** sent once for an `active` booking when `now >= booked_end`. Mentions the late fee only when the tariff at `booked_start` has `late_fee_cents_per_started_h > 0` and the booking is not a club trip.
6. **Once-only:** booking gains `start_reminded` and `late_reminded` (bool, default false, persisted). Set only after a successful send (a failed send retries on the next 5-min tick while the condition holds). A change of `booked_start` clears `start_reminded`; a change of `booked_end` clears `late_reminded`.
7. **Reminder job** runs inside the existing expire timer, after `api_expire_bookings` (so past reservations are closed first).
8. **Run-done mail** content: run id, period, invoices mailed so far / total, mailed by this call, failed invoice numbers, run total, collection date, link `#/admin/abrechnung`.
9. **Deactivation** (admin `PUT /api/admin/users/{id}` with `active:false`) also cancels every `reserved` booking of the user with cancel fee 0 (same step as mandate revoke): a deactivated user cannot log in to start it, and an expired reservation would be billed as unused time. An `active` booking stays and is billed.
10. **Export:** `GET /api/admin/users/{id}/export` (admin) → `200 application/json` with `exported_at`, `user` (public user JSON, never `pw_hash`), `bookings`, `damage_reports` (with `photos`: list of API paths `/api/damage/{id}/photos/{k}`), `mandates` (`mandate_api_json`, masked), `invoices` (each with its `lines`). Sessions, tokens and odometer readings are not personal records of the user and are left out. The web saves it as `ocs-export-<id>.json`.
11. **Backup config** lives in `ocs.conf` (the admin has one file): `backup_recipient` (age public key, starts with `age1`), `backup_mail_to`, `backup_dir` (default `/var/backups/ocs`), `backup_max_mail_bytes` (default `20000000`, size of the encrypted file). `config_load` accepts and validates them; the server does not use them otherwise.
12. **Backup content:** daily `ocs-data-YYYY-MM-DD.tar.gz.age` = `data/` without `photos/` and without `*.tmp`; on Sundays (or with `--photos`) also `ocs-photos-YYYY-MM-DD.tar.age` = `photos/`. Both kept encrypted in `backup_dir` (mode 0600), files older than 30 days deleted. Each file over `backup_max_mail_bytes` is not attached; the mail says so instead. GNU tar exit 1 ("file changed as we read it", e.g. the readings log) counts as success.
13. **Restore check:** `ocs-server -c <conf> check-data` loads the data dir read-only and prints table counts; exit 0 ok, 1 on load failure. `ocs-restore.sh` refuses a non-empty target dir.

## Global Constraints

- All Phase 1–6 Global Constraints hold (`int64_t` UTC seconds, money `int64_t` cents, no float, German UI, English code, `char[]` reject-not-truncate, `-Wall -Wextra -Werror`, ASan/UBSan tests, db copy-before-modify, `web/sw.js` `CACHE` bumped once per task that changes web files).
- Build/test from repo root: `cmake --build build && ctest --test-dir build --output-on-failure`; web: `node --test web/test/`.
- Mails address the user with „du“; local times in mails are Europe/Berlin via `berlin.h`, format `04.11.2026 14:30`.
- Full IBANs never in API responses, exports or mails.
- The backup must never leave the host unencrypted: the script writes only `.age` files to `backup_dir` and only attaches `.age` files; the plaintext tarball is never written to disk (`tar … | age …` pipe).
- Nothing is deployed: no compatibility with older data files; missing new fields take their defaults.

## Review Focus

1. **Server restarts or SMTP is down at reminder time:** the reminder is sent on a later tick while still in its window, and never twice once sent. Test: Task 4 `test_reminder_retried_after_mail_failure_then_once`.
2. **Booking extended after the late reminder:** a new `booked_end` re-arms the reminder for the new end, not immediately. Test: Task 3 `test_put_end_clears_late_reminded` and Task 4 `test_late_reminder_after_extension_waits_for_new_end`.
3. **Admin edits only km/actual times of a done booking:** no "geändert" mail to the member. Test: Task 3 `test_admin_put_km_only_no_mail`.
4. **Backup restored on a fresh host:** decrypts with the admin's identity, the server loads it, content identical. Test: Task 8 `backup_roundtrip.sh` (diff + `check-data`).
5. **Deactivated user's future reservation:** it must not be expired into a full unused-time charge. Test: Task 6 `test_deactivate_cancels_reserved_fee_0`.

---

## File Structure

```
server/src/core/model.h               booking: start_reminded, late_reminded; settings: reminder_before_min
server/src/core/codec.c               new fields (optional, defaults)
server/src/core/notify.{h,c}          NEW: pure mail texts, notify_local_time
server/src/http/api_notify.c          NEW: api_notify_booking, api_send_reminders, api_notify_run_sent
server/src/http/api.h                 declarations of the three glue functions
server/src/http/api_booking.c         hooks + flag reset (user endpoints)
server/src/http/api_admin.c           hooks + flag reset (admin bookings), settings key, deactivation cancel, export
server/src/http/api_mandate.c         cancel-reserved step extracted for reuse
server/src/http/api_billing.c         run-done hook after send
server/src/http/api.c                 export route
server/src/http/server.c              reminder call in expire_cb
server/src/app.h, config.c, main.c    backup_* keys, check-data subcommand
server/test/test_notify.c, test_api_notify.c   NEW
deploy/vps/ocs-backup.sh, ocs-restore.sh, ocs-backup.service, ocs-backup.timer   NEW
deploy/vps/test/backup_roundtrip.sh   NEW (ctest)
deploy/vps/ocs.conf.example, README.md
web/js/views/admin_settings.js, admin_users.js, web/sw.js
docs/spec.md, docs/backlog.md
```

---

### Task 1: Model, codec, `reminder_before_min` setting

**Files:** Modify `server/src/core/model.h`, `server/src/core/codec.c`, `server/src/http/api_admin.c` (settings GET/PUT), `server/test/test_codec.c`, `server/test/test_api_admin.c`, every test that PUTs full settings (add the key; grep `sepa_lead_days` in `server/test/`), `server/fuzz/fuzz_api.c` if it carries a settings body.

**Interfaces (produces):**
```c
/* booking_t gains */ bool start_reminded, late_reminded;   /* default false */
/* settings_t gains */ int reminder_before_min;              /* 0..1440, default 60, 0 = off */
#define REMINDER_MAX_MIN 1440   /* api_admin.c */
```
JSON keys: `start_reminded`, `late_reminded` (booking; optional on load, false), `reminder_before_min` (settings; optional on load, 60; in GET response).

- [ ] **Step 1: Tests:** `test_booking_reminded_roundtrip` (both true survive to/from JSON); `test_booking_reminded_default_false` (JSON without the keys); `test_settings_reminder_default_60`; API `test_settings_put_reminder_ok` (`90` → GET returns 90), `test_settings_put_reminder_missing_400`, `test_settings_put_reminder_out_of_range_400` (`-1` and `1441`).
- [ ] **Step 2:** run, expect FAIL.
- [ ] **Step 3:** implement; PUT reads `reminder_before_min` with the other ints (required).
- [ ] **Step 4:** full `ctest` passes.
- [ ] **Step 5:** commit `feat(notify): reminder flags on bookings, reminder_before_min setting`.

---

### Task 2: Pure mail texts (`core/notify`)

**Files:** Create `server/src/core/notify.{h,c}`, `server/test/test_notify.c`; Modify `server/CMakeLists.txt` (`ocs_add_test(test_notify)`).

**Interfaces (produces, `core/notify.h`):**
```c
typedef enum { NT_CREATED = 0, NT_MODIFIED, NT_CANCELLED, NT_REMIND_START, NT_REMIND_LATE } notify_kind_t;
typedef struct {
  notify_kind_t kind;
  const booking_t *b; const car_t *car;
  const char *user_name, *base_url;
  bool by_admin;                 /* NT_MODIFIED/NT_CANCELLED: actor != booking user */
  int start_tolerance_min;       /* NT_REMIND_START: start allowed from; NT_REMIND_LATE: fee from */
  int64_t late_fee_cents_per_h;  /* NT_REMIND_LATE; 0 = no fee line */
} notify_booking_t;
/* 0 ok, -1 if subject or body does not fit. */
int notify_booking_mail(const notify_booking_t *n, char *subject, size_t scap, char *body, size_t bcap);

typedef struct {
  int64_t run_id; const char *period;          /* "YYYY-MM" */
  size_t n_invoices, n_mailed_total, n_sent_now;
  const int64_t *failed; size_t n_failed;
  int64_t total_cents; const char *collection_date;  /* "YYYY-MM-DD" */
  const char *base_url;
} notify_run_t;
int notify_run_mail(const notify_run_t *r, char *subject, size_t scap, char *body, size_t bcap);

void notify_local_time(int64_t utc, char out[17]);   /* "04.11.2026 14:30", Berlin */
```
Reuse `money_de` (`core/invoice_doc.h`); export the static `period_label` of `invoice_doc.c` as `void period_label(const char *period, char *out, size_t cap)` in `invoice_doc.h` ("2026-10" → "Oktober 2026").

Exact copy (`<…>` = value; `[…]` = line only when the condition holds):

| kind | subject | first paragraph |
|---|---|---|
| NT_CREATED | `Buchung <id> bestätigt` | `deine Buchung ist eingetragen:` |
| NT_MODIFIED | `Buchung <id> geändert` | `deine Buchung wurde geändert:` [+ ` Die Änderung hat die Vereinsverwaltung vorgenommen.` if by_admin] |
| NT_CANCELLED | `Buchung <id> storniert` | `deine Buchung wurde storniert:` [+ ` Die Stornierung hat die Vereinsverwaltung vorgenommen.` if by_admin] |
| NT_REMIND_START | `Erinnerung: Fahrt ab <HH:MM>` | `deine Fahrt beginnt bald:` |
| NT_REMIND_LATE | `Rückgabe überfällig: <car name>` | `deine Buchung ist am <end> abgelaufen, die Fahrt ist aber noch nicht beendet. Bitte bring das Auto zurück und beende die Fahrt unter „Meine Buchungen“.` |

Body layout (LF; mail_build converts):
```
Hallo <user_name>,

<first paragraph>

Auto: <car name> (<plate>)
Von: <notify_local_time(booked_start)>
Bis: <notify_local_time(booked_end)>
[Vereinsfahrt]                                  if club_trip
[Standort: <location>]                          if non-empty and kind != NT_CANCELLED
[Stornogebühr: <money_de>]                      NT_CANCELLED and cancel_fee_cents > 0
[Starten kannst du ab <start - tolerance>.]     NT_REMIND_START
[Ab <end + tolerance> kostet jede angefangene Stunde <money_de(fee)> Verspätungsgebühr.]  NT_REMIND_LATE, fee > 0, !club_trip

Meine Buchungen: <base_url>/#/buchungen
```
Run mail: subject `Abrechnung <Monat Jahr> versendet`; body:
```
Abrechnungslauf <run_id> (<Monat Jahr>)

Rechnungen versendet: <n_mailed_total> von <n_invoices>
Bei diesem Versand: <n_sent_now>
Fehlgeschlagen: <comma-separated numbers | keine>
Summe: <money_de(total_cents)>
Einzugstermin: <DD.MM.YYYY>

[Bitte den Versand für die fehlgeschlagenen Rechnungen wiederholen.]   if n_failed > 0

<base_url>/#/admin/abrechnung
```

- [ ] **Step 1: Tests** (golden, full-string `TEST_ASSERT_EQUAL_STRING`): `test_local_time_cet` (`1762264800` → `04.11.2025 15:00`), `test_local_time_cest` (2026-07-01 10:00 UTC → `01.07.2026 12:00`), `test_created_golden`, `test_modified_by_admin_golden`, `test_cancelled_with_fee_golden` (fee 500 → `Stornogebühr: 5,00 €`, no `Standort`), `test_remind_start_golden` (tolerance 15), `test_remind_late_fee_golden`, `test_remind_late_club_no_fee_line`, `test_run_mail_failed_golden` (failed `{17,18}` → `Fehlgeschlagen: 17, 18` and the retry line), `test_run_mail_no_failed` (`keine`, no retry line), `test_too_small_buffer_returns_minus1`.
- [ ] **Step 2:** run, expect FAIL.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** `ctest -R test_notify` passes.
- [ ] **Step 5:** commit `feat(notify): mail texts for booking events, reminders and billing run`.

---

### Task 3: Booking event mails

**Files:** Create `server/src/http/api_notify.c`, `server/test/test_api_notify.c`; Modify `server/src/http/api.h`, `server/src/http/api_booking.c`, `server/src/http/api_admin.c`, `server/CMakeLists.txt`.

**Interfaces:**
- Consumes: `notify_booking_mail`, `api_send_mail`, `db_tariff_at`.
- Produces (`http/api.h`):
```c
/* Mails the booking's user about kind (NT_CREATED/MODIFIED/CANCELLED). actor_id: who acted.
 * 0 sent, 1 skipped (user missing or inactive), -1 build or send failed. Never touches the db. */
int api_notify_booking(app_t *a, notify_kind_t kind, const booking_t *b, int64_t actor_id);
```
Hooks, each after the successful `db_put_booking` and before the response, result ignored: user POST (`NT_CREATED`), admin POST (`NT_CREATED`), user PUT and admin PUT (`NT_MODIFIED` only if `booked_start`, `booked_end`, `car_id` or `club_trip` differ from the old row), user DELETE and admin DELETE (`NT_CANCELLED`). In both PUT handlers, before the put: `booked_start` changed → `start_reminded = false`; `booked_end` changed → `late_reminded = false`.

- [ ] **Step 1: Tests** (`test_api_notify.c`, fake mailer via `th_mail_count`/`th_last_mail_*`): `test_post_mails_user_created` (subject `Buchung <id> bestätigt`, to = user), `test_admin_post_mails_member_not_admin`, `test_put_extend_mails_modified`, `test_put_club_flag_only_mails_modified`, `test_admin_put_km_only_no_mail` (done booking, only `km_start`/`km_end` → count unchanged), `test_admin_put_times_mail_says_admin` (body contains `Vereinsverwaltung`), `test_delete_mails_cancelled_with_fee` (cancel_fee_cents 500 inside the deadline), `test_mail_failure_keeps_201` (`th_mail_fail(1)`: POST still 201, booking stored), `test_inactive_user_no_mail` (admin POST for an inactive user), `test_put_start_clears_start_reminded`, `test_put_end_clears_late_reminded` (plant the flag true via `db_put_booking`, extend, flag false).
- [ ] **Step 2:** run, expect FAIL.
- [ ] **Step 3:** implement; tariff lookup not needed here (fee only in late reminder).
- [ ] **Step 4:** full `ctest` passes (existing tests that assert `th_mail_count` may need their expected counts raised; adjust, don't weaken).
- [ ] **Step 5:** commit `feat(notify): booking created/modified/cancelled mails`.

---

### Task 4: Start and late-return reminders

**Files:** Modify `server/src/http/api_notify.c`, `server/src/http/api.h`, `server/src/http/server.c` (`expire_cb`), `server/test/test_api_notify.c`.

**Interfaces (produces):**
```c
/* Sends due start/late reminders (rules: plan builder calls 4–6) and persists the flag after each
 * successful send. Returns number sent, -1 on storage error (stops). Mail failures are skipped
 * (flag stays false, retried next call). */
int api_send_reminders(app_t *a, int64_t now);
```
`expire_cb`: after `api_expire_bookings`, call `api_send_reminders(a, now)`; log `ocs-server: api_send_reminders failed` on -1. Loop over indices, copy, put (db contract). Late fee = `db_tariff_at(car_id, booked_start)->late_fee_cents_per_started_h` (0 if no tariff).

- [ ] **Step 1: Tests** (setting 60 min, tolerance 15): `test_start_reminder_in_window` (now = start − 30 min → 1 mail `Erinnerung: Fahrt ab HH:MM`, flag true), `test_start_reminder_not_before_window` (start − 61 min → 0), `test_start_reminder_once` (two calls → 1 mail), `test_start_reminder_off_when_0`, `test_start_reminder_skipped_when_created_in_window` (created at start − 10 min), `test_no_start_reminder_after_start`, `test_late_reminder_at_end` (active, now = end → mail `Rückgabe überfällig: …`, flag true), `test_late_reminder_fee_from_tariff` (tariff fee 300 → body contains `3,00 €`), `test_late_reminder_after_extension_waits_for_new_end` (reminded, user PUT later end, call at old end + 1 min → 0 mails; at new end → 1), `test_reminder_retried_after_mail_failure_then_once` (`th_mail_fail(1)` → 0 sent, flag false; `th_mail_fail(0)` → 1; again → 0), `test_reminder_inactive_user_skipped`, `test_reminder_flag_persisted` (reload db from dir, flag true).
- [ ] **Step 2:** run, expect FAIL.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** full `ctest` passes.
- [ ] **Step 5:** commit `feat(notify): start and late-return reminders in the 5-min job`.

---

### Task 5: "Billing run done" admin mail

**Files:** Modify `server/src/http/api_notify.c`, `server/src/http/api.h`, `server/src/http/api_billing.c` (`h_admin_billing_send`), `server/test/test_api_billing.c`, `server/test/helpers.{h,c}` (`void th_mail_fail_to(const char *to);` — fake mailer fails only for that recipient, NULL = off, reset by `th_app`).

**Interfaces (produces):**
```c
/* Mails notify_run_mail to every active admin. 1 if at least one sent, 0 no admin, -1 all failed. */
int api_notify_run_sent(app_t *a, const billing_run_t *run, const invoice_t *invs, size_t n,
                        size_t sent_now, const int64_t *failed, size_t n_failed);
```
Call in `h_admin_billing_send` after the mail loop, before building the response (also when `sent_now == 0` on a retry, not when the call was refused with 409/500). `n_mailed_total` = invoices with `mailed_at != 0`; `total_cents` = sum over `invs`.

- [ ] **Step 1: Tests:** `test_send_mails_admins_run_done` (two active admins, one inactive → 2 extra mails; last subject `Abrechnung <Monat Jahr> versendet`, body `Rechnungen versendet: 2 von 2`), `test_send_failed_listed_in_run_mail` (`th_mail_fail_to(<member email>)` → run mail still reaches the admins, body contains `Fehlgeschlagen: <that invoice number>` and the retry line), `test_send_refused_no_run_mail` (409 `not_final` → mail count unchanged).
- [ ] **Step 2:** run, expect FAIL.
- [ ] **Step 3:** implement. Existing send tests that count mails: raise the expected counts by the admin count.
- [ ] **Step 4:** full `ctest` passes.
- [ ] **Step 5:** commit `feat(billing): billing run done mail to admins`.

---

### Task 6: Deactivation cancels reservations, GDPR export, retention

**Files:** Modify `server/src/http/api_mandate.c`, `server/src/http/api_internal.h`, `server/src/http/api_admin.c`, `server/src/http/api.c` (route), `server/test/test_api_admin.c`.

**Interfaces (produces):**
```c
/* api_internal.h. Cancels every RESERVED booking of user_id: cancel_fee_cents 0, state CANCELLED,
 * cancelled_at/modified_at now, modified_by actor_id. No mails. Count or -1 on storage error. */
int api_cancel_reserved(app_t *a, int64_t user_id, int64_t actor_id, int64_t now);
void h_admin_user_export(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
```
Route `{"GET", "/api/admin/users/*/export", h_admin_user_export, MIN_ADMIN, BODY_JSON}`. `api_mandate_revoke` calls `api_cancel_reserved` for its booking step (behaviour unchanged). `h_admin_user_put` on deactivation: put user, delete sessions, then `api_cancel_reserved` (500 `storage` on -1). Export body: builder call 10; bookings via `booking_to_json`, damage via existing codec plus `photos` array, invoices via the invoice codec plus `lines` (`invoice_line` codec, sorted by booking start). 404 for unknown user.

- [ ] **Step 1: Tests:** `test_deactivate_cancels_reserved_fee_0` (reserved booking → cancelled, fee 0; then `api_expire_bookings` after its end leaves it cancelled), `test_deactivate_keeps_active_booking`, `test_revoke_still_cancels_reserved` (existing mandate tests pass unchanged), `test_export_contains_all_sections` (keys `exported_at user bookings damage_reports mandates invoices`; user has no `pw_hash`), `test_export_iban_masked` (`strstr(body, "DE89370400440532013000") == NULL`, contains `iban_masked`), `test_export_photo_paths` (report with 2 photos → `/api/damage/<id>/photos/1` and `/2`), `test_export_unknown_user_404`, `test_export_user_role_403`, `test_deactivate_keeps_invoices_and_mandates` (finalized invoice + mandate survive deactivation and `db_prune`; export still lists them).
- [ ] **Step 2:** run, expect FAIL.
- [ ] **Step 3:** implement.
- [ ] **Step 4:** full `ctest` passes; `fuzz_api` still builds if `OCS_FUZZ` build dir exists (route table).
- [ ] **Step 5:** commit `feat(admin): GDPR export, deactivation cancels reservations`.

---

### Task 7: Backup config keys, `check-data` subcommand

**Files:** Modify `server/src/app.h`, `server/src/config.c`, `server/src/main.c`, `server/test/test_config.c`, `deploy/vps/ocs.conf.example`.

**Interfaces (produces):**
```c
/* config_t gains */ char backup_recipient[128], backup_mail_to[128], backup_dir[256];
                     int64_t backup_max_mail_bytes;   /* default 20000000 */
```
Validation: `backup_recipient` must start with `age1`, only `[a-z0-9]`; `backup_mail_to` passes `email_normalize`; `backup_max_mail_bytes` integer 1..100000000; `backup_dir` default `/var/backups/ocs`. All optional for the server. `ocs-server -c <conf> check-data`: `db_load` the data dir, print `users N, bookings N, invoices N, mandates N` to stdout, exit 0; load failure exit 1. Usage line gains `| check-data`. It never writes (no `db_put_*`).

- [ ] **Step 1: Tests:** `test_backup_keys_parsed`, `test_backup_defaults` (dir and max bytes), `test_backup_recipient_invalid` (`ssh-ed25519 …` → -1), `test_backup_mail_to_invalid`, `test_backup_max_bytes_out_of_range`.
- [ ] **Step 2:** run, expect FAIL.
- [ ] **Step 3:** implement; document the four keys (commented) in `ocs.conf.example`.
- [ ] **Step 4:** `ctest` passes; manual: `build/server/ocs-server -c <tmp conf> check-data` on an empty dir prints zero counts, exit 0.
- [ ] **Step 5:** commit `feat(ops): backup config keys, check-data subcommand`.

---

### Task 8: Backup and restore scripts, systemd timer

**Files:** Create `deploy/vps/ocs-backup.sh`, `deploy/vps/ocs-restore.sh`, `deploy/vps/ocs-backup.service`, `deploy/vps/ocs-backup.timer`, `deploy/vps/test/backup_roundtrip.sh`; Modify `server/CMakeLists.txt`.

**Interfaces:**
- `ocs-backup.sh [-c /etc/ocs/ocs.conf] [--photos]` — bash, `set -euo pipefail`, `umask 077`. Reads `data_dir smtp_url smtp_user smtp_pass mail_from backup_*` from the conf (same `key = value` / `#` syntax). Missing `backup_recipient` or `backup_mail_to` → exit 2 with message. Steps: (1) `tar -C <data_dir> --exclude=./photos --exclude='*.tmp' -czf - . | age -r <recipient> -o <dir>/ocs-data-<date>.tar.gz.age` (tar exit 1 tolerated, ≥2 fails); (2) if `--photos` or `date +%u` = 7 and `photos/` exists: same for `ocs-photos-<date>.tar.age` (no gzip; JPEGs); (3) delete `ocs-*.age` in `backup_dir` older than 30 days (`find -mtime +30`); (4) one mail, subject `open-carshare Backup <date>`, multipart/mixed, each `.age` ≤ `backup_max_mail_bytes` attached base64 (76 cols), otherwise a body line `<file>: <bytes> Bytes, zu groß für die Mail, nur lokal unter <dir> gespeichert.`; (5) send via `curl --url <smtp_url> --mail-from <envelope> --mail-rcpt <to> --upload-file <msg>` with `--ssl-reqd` unless host is localhost/127.0.0.1, credentials passed via `-K` on a file descriptor (never in argv). Test hook: env `OCS_BACKUP_MAIL_FILE=<path>` writes the message there instead of curl. Exit non-zero if any step fails (local files stay).
- `ocs-restore.sh <archive.age> <identity-file> <target-dir>` — refuses an existing non-empty target (exit 2); `age -d -i <identity> <archive> | tar -x -C <target>` (`-z` for `.tar.gz.age`).
- `ocs-backup.service`: `Type=oneshot`, `User=ocs`, `ExecStart=/opt/ocs/ocs-backup.sh -c /etc/ocs/ocs.conf`, `ProtectSystem=strict`, `ReadWritePaths=/var/backups/ocs`, `PrivateTmp=yes`, `NoNewPrivileges=yes`. `ocs-backup.timer`: `OnCalendar=*-*-* 03:15`, `Persistent=true`.
- CMake: `add_test(NAME backup_roundtrip COMMAND bash ${CMAKE_SOURCE_DIR}/deploy/vps/test/backup_roundtrip.sh $<TARGET_FILE:ocs-server>)` with `set_tests_properties(backup_roundtrip PROPERTIES SKIP_RETURN_CODE 77)`; the script exits 77 when `age`/`age-keygen` are missing.

- [ ] **Step 0:** `sudo apt install age` on the dev machine (this test must run, not skip, before the phase is done).
- [ ] **Step 1: Test** `backup_roundtrip.sh <ocs-server>`: tmp dir with `data/` (users.jsonl, bookings.jsonl, a `x.tmp`, `photos/r1-1.jpg`), conf pointing at it, `age-keygen -o key.txt`, recipient from it. Cases, each failing the script with a message: (a) daily run with `OCS_BACKUP_MAIL_FILE` → exactly one `ocs-data-*.tar.gz.age` in `backup_dir`, mode 600, no `ocs-photos-*`; mail file has `Content-Disposition: attachment; filename="ocs-data-<date>.tar.gz.age"`; (b) attachment extracted from the mail (base64 between the part headers and the boundary), restored with `ocs-restore.sh` into an empty dir → `diff -r` equal to `data/` minus `photos/` and `x.tmp`; `<ocs-server> -c <conf for restored dir> check-data` exits 0; (c) `--photos` → `ocs-photos-*.tar.age` restores to identical `photos/`; (d) `backup_max_mail_bytes = 10` → no attachment, body contains `zu groß für die Mail`; (e) a planted `ocs-data-2000-01-01.tar.gz.age` with `touch -d '40 days ago'` is deleted, one at 10 days kept; (f) `grep -c` of the plaintext `users.jsonl` content in `backup_dir` and the mail file is 0; (g) `ocs-restore.sh` into a non-empty dir exits 2.
- [ ] **Step 2:** run `ctest -R backup_roundtrip`, expect FAIL (scripts missing).
- [ ] **Step 3:** implement scripts and units; `shellcheck` them if installed.
- [ ] **Step 4:** `ctest -R backup_roundtrip --output-on-failure` passes (not "Skipped").
- [ ] **Step 5:** commit `feat(ops): encrypted nightly backup by mail, restore script, systemd timer`.

---

### Task 9: Web — reminder setting, export, deactivation hint

**Files:** Modify `web/js/views/admin_settings.js`, `web/js/views/admin_users.js`, `web/sw.js` (bump `CACHE` once), `web/test/admin.test.mjs` if a pure helper is added.

**Interfaces:** consumes `reminder_before_min` (Task 1), `GET /api/admin/users/{id}/export` (Task 6).

- [ ] **Step 1:** settings form: numeric field „Erinnerung vor Fahrtbeginn (Minuten, 0 = aus)“, sent as `reminder_before_min`, refilled from the response.
- [ ] **Step 2:** user edit dialog: button „Daten exportieren“ → `fetch` the export, save as `ocs-export-<id>.json` (Blob + `a[download]`); error via the existing message box.
- [ ] **Step 3:** unchecking „aktiv“ on save asks via the existing confirm pattern: „Konto deaktivieren? Offene Reservierungen werden kostenlos storniert.“.
- [ ] **Step 4:** `node --test web/test/` passes; manual in the browser (`run` skill): set 90 min, reload, value stays; export downloads JSON without full IBAN; deactivating a user with a future reservation shows it cancelled under admin bookings.
- [ ] **Step 5:** commit `feat(web): reminder setting, user data export, deactivation hint`.

---

### Task 10: Docs

**Files:** Modify `docs/spec.md`, `deploy/vps/README.md`, `docs/backlog.md`.

- [ ] **Step 1: spec:** §4 booking (`start_reminded`, `late_reminded`), settings (`reminder_before_min`); §10 rewritten with builder calls 1–8 and the subjects from Task 2; §12 with builder calls 9–13 and the product-owner decisions; §13 `GET /api/admin/users/{id}/export`; §15 Phase 7 gets `Plan: docs/superpowers/plans/2026-11-03-phase7-notifications-ops.md` (mark done only at merge).
- [ ] **Step 2: README:** section 9 „Interim backup“ replaced by „Backup“: `apt install age`; admin creates the key pair on their own machine (`age-keygen -o ocs-backup.key`, keep it offline; the public `age1…` line goes into `backup_recipient`); install `ocs-backup.sh`/`ocs-restore.sh` to `/opt/ocs/`, `install -d -o ocs -g ocs -m 0700 /var/backups/ocs`, units, `systemctl enable --now ocs-backup.timer`; test run `sudo systemctl start ocs-backup.service` and check the mail; remove the old root crontab line. New section „Restore“: stop service, `ocs-restore.sh` into an empty dir, `ocs-server -c … check-data`, swap dir, `chown -R ocs:ocs`, start. Note: mail providers often cap at 20–25 MB; photos over the cap stay local only — copy them off the VPS by other means.
- [ ] **Step 3: backlog:** remove the `[P7]` interim-backup item.
- [ ] **Step 4:** full `ctest` and `node --test web/test/` pass.
- [ ] **Step 5:** commit `docs: phase 7 notifications and backup`.

**Acceptance (product owner, manual, after deploy):** a real nightly backup mail arrives, and its attachment restores with `ocs-restore.sh` + `check-data` on another machine.
