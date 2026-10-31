# open-carshare – Specification (draft)

Status: draft, all known questions resolved (2026-10-30).

## 1. Scope

Booking portal for a small carsharing Verein: 1–5 cars, 20–50 users.

Phase scope: **VPS deployment only.** ESP32 deployment is deferred; the design
keeps the server portable (C, few dependencies, file store) but nothing
ESP32-specific is implemented now.

UI language: German only. Code, docs, identifiers: English.

## 2. Architecture

```
Browser / PWA  --HTTPS-->  Caddy (TLS, Let's Encrypt)  --HTTP localhost-->  ocs-server (C)
                                                                              |-- data/   (files)
                                                                              |-- MyGeotab API (HTTPS out)
                                                                              |-- SMTP relay (mail out)
```

- `ocs-server`: single C binary, single-threaded event loop. All requests are
  serialized, so booking conflict checks need no locks.
- Static web client (vanilla HTML/JS/CSS, no build step) served by `ocs-server`.
- PWA: `manifest.json` + service worker, installable from the browser.

### Dependencies

| Purpose | Library | Why |
|---|---|---|
| HTTP server | Mongoose (GPLv2) | portable to ESP-IDF later |
| JSON | cJSON | small, ships with ESP-IDF |
| Password hashing | Monocypher (Argon2id) | single file, portable |
| HTTPS client (Geotab) | libcurl | VPS only, behind `platform/` interface |
| Mail | SMTP via libcurl to relay | behind `platform/` interface |
| Tests | Unity | also runs on ESP32 |

### Repo layout

```
server/
  src/
    main.c
    http/       routing, request parsing, sessions, static files
    core/       pure logic: booking rules, pricing, billing, sepa, pdf (no I/O)
    store/      store.h + file backend
    auth/       password hashing, sessions, roles
    platform/   posix/: time, fs, rng, http client, mail
  test/
  third_party/
web/            index.html, app.js, style.css, manifest.json, sw.js
deploy/vps/     Caddyfile, systemd unit, backup script
docs/
```

`core/` has no I/O and is fully unit-tested.

## 3. Conventions

- Time: `int64_t` Unix epoch seconds, UTC. Displayed in `Europe/Berlin`.
  Pricing granularity: minutes.
- Money: `int64_t` cents. No floating point anywhere in pricing/billing.
- Rounding: per pricing component, half-up, to whole cents.

## 4. Data model

### user
id, email (login), name, street, zip, city, pw_hash, role (`admin`|`user`),
active, account_holder, iban, bic (optional), mandate_ref, mandate_date,
license_checked_at, may_club_trip (default false, set by admin only),
created_at.

### car
id, name, plate, active, location_text, geotab_device_id, keysafe_id.

### keysafe
id, secret (encrypted at rest), algorithm params (step, digits, drift).
PIN algorithm: pluggable, delivered later:
`int access_pin(const keysafe_t*, const booking_t*, int64_t now, char *out, size_t n)`.

### tariff (per car, versioned)
car_id, valid_from, rate_day_cents_per_h, rate_night_cents_per_h,
night_start (minutes after midnight, default 20:00), night_end (default 06:00),
rate_km_cents, unused_factor_permille (default 500),
time_cap_cents_per_day, combined_cap_cents_per_day,
late_fee_cents_per_started_h.

Pricing uses the tariff valid at the booking's booked start.

### booking
id, car_id, user_id, booked_start, booked_end, state
(`reserved`|`active`|`done`|`cancelled`), actual_start, actual_end,
km_start, km_end, km_source (`geotab`|`manual`), km_pending,
cancelled_at, club_trip (default false), invoice_id, created_at, modified_at,
modified_by.

### damage_report
id, booking_id, phase (`start`|`end`), text, photo files (0..n), created_at.
Photos are resized client-side to ≤1600 px JPEG before upload and stored as
files under `data/photos/`.

### invoice
number (gapless sequence, assigned at finalize), user_id, period (YYYY-MM),
line items, total_cents, billing_run_id, mailed_at.

### billing_run
id, period, state (`draft`|`final`), created_at, finalized_at,
collection_date, sepa_file.

### settings
association name/address, creditor_id (Gläubiger-ID), creditor IBAN/BIC,
start_tolerance_min (15), cancel_deadline_min (0), cancel_fee_cents (0),
sepa lead days, invoice footer text, SMTP config, Geotab credentials.

## 5. Booking rules

- No overlap of bookings on the same car (state `reserved` or `active`).
  The interval of an active booking that is running late counts as
  occupied up to now.
- Users may only **extend** a booking (earlier `booked_start` before the
  start, later `booked_end` anytime), and only if no overlap. Users may never
  shorten or move a booking; to reduce it they cancel it and book anew. This
  prevents blocking a car for days and shortening it shortly before the trip.
- **Cancel** is allowed before start only. Cancelling within
  `cancel_deadline_min` before start costs `cancel_fee_cents` (both default
  0, raised only if cancelling is abused).
- **Start** allowed from `booked_start - start_tolerance_min` until
  `booked_end`. **Return** allowed anytime after start.
- Start and return procedures ask for a damage report (optional; "no damage"
  is one tap).
- Never started booking: whole booked time is billed as unused time.
- Admin may modify any booking in any state, including actual times and km.

### Club trips (Vereinsfahrten)
- Members with `user.may_club_trip` (car maintainers, Vorstand; set by admin
  only) may mark a booking as `club_trip` for internal association trips.
  Otherwise it is a normal booking (overlap, tolerance, extend-only, ...).
- Create with `club_trip: true` without the permission: 403 `forbidden`.
- A user may change `club_trip` on an own booking only while
  `now < booked_start` (afterwards 400 `not_modifiable`). Setting it needs the
  permission; clearing it does not. A flag-only change (times unchanged) is a
  valid update. Omitted `club_trip` means unchanged.
- Admin may set `club_trip` on any booking at any time, regardless of the
  booked user's permission. (Admin API only; an admin using the user endpoints needs
  `may_club_trip` like everyone else.)
- Revoking `may_club_trip` leaves existing bookings unchanged.
- Calendar entries of other users stay `{start,end,own:false}`; own entries and
  the booking JSON include `club_trip`.

### Calendar visibility
- User: own bookings + other bookings as "belegt", only from now on, no names.
- Admin: everything.

## 6. Pricing

```
B = [B0, B1]  booked interval
A = [A0, A1]  actual interval (empty if never started)

active   = A                       (includes early start / late return)
unused   = B \ A                   (booked but not used)
late_min = max(0, A1 - B1)

minute price:
  active minute: rate(t) / 60
  unused minute: rate(t) / 60 * unused_factor
  rate(t) = night rate if local time t in [night_start, night_end), else day rate

span       = B ∪ A  (from min(B0, A0) to max(B1, A1))
days       = number of calendar days (local) touched by span
time_cost  = sum over calendar days d of min(time_cost_d, time_cap)
km_cost    = km * rate_km
late_min   = max(0, A1 - (B1 + start_tolerance_min))
late_fee   = ceil(late_min / 60) * late_fee_per_started_h
total      = min(time_cost + km_cost, days * combined_cap) + late_fee + cancel_fee
```

Rules:
- A day is a calendar day 00:00–24:00 local time (DST days have 23/25 h).
- Night rate applies per minute that falls in the night window (local time).
- Unused time is priced at the rate of its actual clock time × factor.
- Time cap applies per calendar day to the day's whole time cost
  (active + unused).
- Combined cap applies to the booking as a whole: `days × combined_cap`.
- Club trip (`club_trip`): total = 0 (time, km, late fee and cancel fee
  all 0; `br_cancel_fee` returns 0 for user and admin cancel).
- Late fee counts started hours beyond `B1 + start_tolerance_min` and is
  outside both caps. Returns within the tolerance are free of late fee
  (the extra minutes are still billed as active time).

## 7. Car access

PIN shown in the client only for an `active` booking (after the start
procedure), computed server-side via `access_pin()`. Keysafe secrets are
encrypted at rest with a key from the server config file (not in `data/`).

## 8. Odometer (Geotab)

- MyGeotab JSON-RPC API: `Authenticate`, then `Get` `StatusData` with
  diagnostic `DiagnosticOdometerId`, device filter, narrow time window,
  `resultsLimit`.
- On start/return: record time immediately, set `km_pending`.
- Background job (every 5 min) resolves pending km from the first odometer
  value at/after start and the last at/before return. Retries until found.
- Admin can enter km manually (`km_source = manual`).
- Billing run refuses to finalize while any booking of the period has
  `km_pending`.

## 9. Billing run (admin)

1. Admin picks period (month). Bookings with `actual_end` (or `booked_end`
   if never started) inside the period are included.
2. Draft: per user, line items and totals computed. Admin reviews and may
   edit bookings, then recompute.
3. Finalize: invoice numbers assigned (gapless), bookings locked
   (`invoice_id` set), SEPA file generated.
4. Send: invoice PDFs mailed to users. The mail is the SEPA pre-notification
   (amount, collection date, mandate ref, creditor ID).
5. Admin downloads the SEPA XML and imports it into Hibiscus.

A club trip appears on the invoice as a line with date/times and km, amount
0 EUR, marked "Vereinsfahrt". A user whose month has only club trips gets a
0 EUR invoice listing them: it takes the next invoice number and is mailed
like any other, but has no SEPA entry, the mail carries no pre-notification,
and it needs no mandate. Users with no invoice lines at all get no invoice.
Users with a total above 0 and without a valid mandate block finalization
(listed in the draft).

### SEPA

Format: **`pain.008.001.02`** (SEPA Core Direct Debit, Sammellastschrift),
imported into Hibiscus. (pain.001 is a credit transfer and does not apply.)

- Mandates are managed in the portal, independent of jVerein:
  `mandate_ref` (generated, e.g. `OCS-000012`), `mandate_date`, account
  holder, IBAN, optional BIC.
- Mandate is given **online** by the user: user enters account holder and
  IBAN, reads the mandate text, confirms. Stored as evidence: timestamp,
  client IP, mandate text version. Changing IBAN or holder creates a new
  mandate. (Online mandates are weaker evidence than paper; chargeback risk
  accepted by the Verein.)
- Sequence type: always `RCUR` (allowed for the first collection since 2016).
- IBAN validated with mod-97 on input.

### Invoice PDF
Minimal built-in PDF writer (standard Helvetica, WinAnsi encoding for
umlauts), one or more pages: association header, user address, invoice
number/date, period, one line per booking (date, times, hours day/night,
unused, km, fees), total, footer text (configurable), SEPA pre-notification
block.

## 10. Notifications (mail only)

Booking created / modified / cancelled, reminder before start (configurable,
default 1 h), late return reminder, invoice. Admin: billing run done,
Geotab sync failing.

## 11. Security

- Argon2id password hashing, session token (random 256 bit) in `HttpOnly`,
  `Secure`, `SameSite=Strict` cookie.
- Login rate limiting per IP and account.
- Password reset via mail link (single use, 1 h).
- All input parsed with length limits; parsers fuzzed (libFuzzer + ASan).
- `ocs-server` binds to localhost only; Caddy terminates TLS.

## 12. Data and backup

- `data/` on disk: one JSON-lines file per table, rewritten via
  write-temp + `rename()`. All data loaded into RAM at start.
- Nightly backup: tarball of `data/` (without photos), kept 30 days on the
  host, and mailed to the admin **encrypted** (age, admin's public key in
  config). The backup contains IBANs and password hashes and must never
  travel by mail unencrypted. Photos are backed up weekly the same way if
  under the mail size limit.
- GDPR: user data export (admin), deactivation; invoices retained 10 years.

## 13. API (JSON)

```
POST /api/login | /api/logout | /api/password-reset
GET/PUT /api/me
GET  /api/cars
GET  /api/calendar?car=&from=&to=
GET  /api/bookings?scope=mine|upcoming
POST /api/bookings                PUT /api/bookings/{id}     DELETE /api/bookings/{id}
POST /api/bookings/{id}/start     POST /api/bookings/{id}/return
POST /api/bookings/{id}/damage    (multipart photo)
GET  /api/bookings/{id}/pin
admin:
  /api/admin/users, /cars, /keysafes, /tariffs, /bookings, /settings
  POST /api/admin/billing?period=    GET /api/admin/billing/{id}
  POST /api/admin/billing/{id}/finalize | /send
  GET  /api/admin/billing/{id}/sepa.xml
```

## 14. Deferred

ESP32 port (LittleFS store, own ACME client with DNS-01 via deSEC, dynamic
DNS, flash encryption), keysafe PIN algorithm, app store wrappers.

## 15. Phases

Each phase is started in a fresh session. Start of a phase: read this spec
and `docs/backlog.md`, clarify open questions with the product owner, write
the plan to `docs/superpowers/plans/YYYY-MM-DD-phaseN-<topic>.md`, implement
it on a branch `phaseN-<topic>`, merge to `main`, then mark the phase done
here and update the backlog. Items in the backlog tagged with the phase are
part of its scope.

### Phase 1: Booking core — done (merged 2026-10-31)
Plan: `docs/superpowers/plans/2026-10-30-phase1-booking-core.md`. Server
skeleton, JSONL store, auth (login, sessions, invite, password reset, rate
limits), users and cars admin, booking rules (§5), calendar, PWA, VPS
deployment with interim tar backup. Includes club trips (§5 Club trips).

### Phase 2: Trip lifecycle
Depends on: Phase 1.
- `POST /api/bookings/{id}/start` and `/return`: state `reserved` →
  `active` → `done`, `actual_start`/`actual_end` recorded, early start
  allowed when the car is free, `start_tolerance_min` applied.
- Calendar and overlap rules for `active` bookings, including overdue ones
  (an overdue `active` booking occupies the car until returned).
- Damage reports at start and return (§4 damage_report): text and photos,
  client-side resize, multipart upload, files under `data/photos/`, admin
  view.
- Booking history for users (past bookings with actual times) and admin.
- Manual km entry by admin (`km_source = manual`); the Geotab path comes in
  Phase 3, so `km_pending` is set on return and resolved manually for now.
- UI: start/return flow in "Meine Buchungen", no "Stornieren" on started
  bookings.
Done when: a booking can be started, returned, reported on and shows up in
history with actual times and km.

### Phase 3: Odometer (Geotab)
Depends on: Phase 2.
- MyGeotab client (§8) in `platform/posix/` (libcurl), credentials in
  settings, `geotab_device_id` per car.
- Background job every 5 min resolves `km_pending`; retries until found.
- Admin mail when the Geotab sync keeps failing.
- Tests against a recorded/fake Geotab endpoint.
Done when: returned bookings get km automatically, admin can still
override manually.

### Phase 4: Tariffs and pricing
Depends on: Phase 2 (actual times); Phase 3 km optional (manual km works).
- Tariff entity (§4), versioned per car, admin CRUD.
- Pricing engine (§6) as a pure function in `core/`: day/night per minute,
  unused factor, per-day time cap, combined cap, late fee, cancel fee,
  club trip = 0. Exhaustive unit tests including DST days and caps.
- Price shown per booking in history (user) and admin bookings.
Done when: every `done` or `cancelled` booking has a computed price that
matches hand-calculated test cases.

### Phase 5: SEPA mandates
Depends on: Phase 1.
- User enters account holder, IBAN (mod-97), optional BIC; reads the
  versioned mandate text and confirms online (§9 SEPA).
- `mandate_ref` generation (`OCS-000012`), `mandate_date`, evidence
  (timestamp, client IP, text version); changing IBAN/holder creates a new
  mandate.
- Admin: mandate status per user; settings for creditor ID, IBAN, BIC.
Done when: users can give and replace a mandate and admin sees who lacks one.

### Phase 6: Billing run
Depends on: Phases 4 and 5 (Phase 3 for the `km_pending` gate).
- Billing run per month (§9): draft, admin edits and recompute, finalize
  (gapless invoice numbers, bookings locked via `invoice_id`), send.
- Finalize blocked by `km_pending` and by users with total > 0 without a
  valid mandate; 0 EUR invoices for months with only club trips.
- Invoice PDF writer (§9 Invoice PDF), invoice mail with SEPA
  pre-notification.
- SEPA `pain.008.001.02` export for Hibiscus, `sepa lead days` setting.
Done when: a month can be billed end-to-end and the XML imports into
Hibiscus.

### Phase 7: Notifications and operations
Depends on: Phase 2 (reminders reference start/return); can run before
Phase 6 except the "billing run done" mail.
- Booking mails: created, modified, cancelled; reminder before start
  (default 1 h); late return reminder (§10).
- Nightly encrypted backup by mail (age, admin public key), 30-day local
  retention, weekly photo backup (§12); replaces the interim tar cron.
- GDPR: user data export (admin), deactivation; invoice retention 10 years.
Done when: all §10 mails are sent and the encrypted backup arrives and
restores.

### Later (see §14)
Keysafe entity and PIN (`access_pin`, §7) once the algorithm is delivered;
ESP32 port; app store wrappers.
