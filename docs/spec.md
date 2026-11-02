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
                                                                              |-- SMTP relay (mail out)
Teltonika trackers  --TCP (Codec 8 Extended)-->  ocs-server tracker port (§8)
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
    platform/   posix/: time, fs, rng, mail
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
active, license_checked_at, may_club_trip (default false, set by admin only),
created_at.

### mandate
id, user_id, holder (account holder, max 70 UTF-8 code points), iban, bic
(optional, 8 or 11 characters), status (`active`|`replaced`|`revoked`),
created_at, ended_at, ended_by (user id), creditor_name and creditor_id
(snapshot of the settings at the time), client_ip, text_version.
IBAN, BIC and creditor ID are stored normalized (spaces removed, uppercase).
The reference is derived, not stored: `OCS-%06d` of the id (`OCS-000012`).
The current mandate of a user is the `active` row (newest id). Old rows are
kept as evidence. Stored in `data/mandates.jsonl`; a missing file means no
mandates.

### car
id, name, plate, active, location_text, tracker_imei (15 digits, optional,
unique), keysafe_id.

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
km_start, km_end, km_source (`telematics`|`manual`), km_pending,
cancelled_at, club_trip (default false), invoice_id, created_at, modified_at,
modified_by.

### damage_report
id, booking_id, user_id, phase (`start`|`end`), text (required, < 1024 bytes),
n_photos (0..10), created_at. Photos are resized client-side to ≤1600 px JPEG
before upload; each upload is one raw `image/jpeg` body of at most 2 MiB
(2097152 bytes) starting with `FF D8 FF`, stored as
`data/photos/r<report id>-<k>.jpg` (k = 1..n_photos). "Keine Schäden" stores
nothing; only real damage creates a report.

### invoice
number (gapless sequence, assigned at finalize), user_id, period (YYYY-MM),
line items, total_cents, billing_run_id, mailed_at.

### billing_run
id, period, state (`draft`|`final`), created_at, finalized_at,
collection_date, sepa_file.

### settings
String keys `assoc_name`, `assoc_street`, `assoc_zip`, `assoc_city`,
`creditor_id` (Gläubiger-ID), `creditor_iban`, `creditor_bic` (the admin
settings PUT requires all keys), start_tolerance_min (15), cancel_deadline_min (0), cancel_fee_cents (0),
sepa lead days, invoice footer text, SMTP config.

### odometer_reading
car_id, ts (device GNSS time, epoch seconds), odometer_m (tracker total
odometer, metres), received_at. Nothing else from the tracker is stored.
Kept 90 days, then pruned.

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

Cancelled bookings cost only `cancel_fee` (no time or km cost); the formula
prices `done` bookings, a `done` booking that never started has all booked
time unused.
Granularity: whole minutes (actual times truncated); time cost rounded half
up once per local day; cap 0 = no cap.

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

## 8. Odometer (Teltonika telematics)

Hardware: one Teltonika FMx tracker per car in the OBD port (4G model, e.g.
FMC003 or its successor; no 2G-only models such as FMB003, German 2G networks
shut down in 2028), IoT SIM (e.g. 1NCE). No vendor cloud, no Traccar: the
tracker sends to `ocs-server` directly. Km come from the tracker's GNSS-based
total odometer (AVL ID 16 "Total Odometer", metres; verify against the model's
AVL ID list), set once to the dashboard reading. A deviation of a few percent
from the dashboard is accepted for billing. The OEM odometer (AVL ID 389) is
not used (unconfirmed for the Leapmotor T03).

Ingestion:
- `ocs-server` listens on a dedicated TCP port (`tracker_port` in the config
  file), Teltonika protocol with Codec 8 Extended (`0x8E`).
- Handshake: the tracker sends its IMEI; the server answers `0x01` if it
  matches the `tracker_imei` of a car (active or deactivated, so pending
  trips of a deactivated car still resolve), else `0x00` and closes.
- AVL data packet: length and CRC-16/IBM checked; the server acknowledges the
  record count only after the readings are stored, so the tracker resends on
  failure. Packet size, connection count and idle time are limited.
- From each record only the timestamp and the odometer element are kept.
  GPS positions and all other I/O elements are discarded on arrival and never
  stored or logged (no movement profiles of members).
- The packet parser lives in `core/` (pure, no I/O) and is fuzzed.
- Storing a packet must not rewrite the whole readings file (append; pruning
  rewrites it).

Tracker configuration (documented in `deploy/vps/README.md`): server domain
and port, TCP, Codec 8 Extended, APN of the SIM, records on movement
start/stop (movement source: movement sensor/GNSS, since ignition detection is
unreliable on EVs), records sorted oldest first, a record at least every
5 min while moving and every 60 min while parked. The parked record is what lets a trip resolve after the
car is returned (see below); the 12 V battery drain of the parked mode is
checked in the hardware acceptance.

Resolution:
- On start/return: record time immediately, set `km_pending` on return.
- Background job (every 5 min) resolves each `km_pending` booking of a car
  with `tracker_imei`:
  - Wait until a reading with `ts > actual_end` exists that was received at
    least 600 s ago (`received_at <= now - 600`): buffered records from dead
    zones have arrived, also when the tracker sends a backlog newest first or
    across several packets; normally the next parked record, at most 60 min
    after return. Until then the booking stays pending.
  - start = first reading with `actual_start <= ts <= actual_end`,
    end = last reading in the same window.
  - No reading inside the trip: if the last reading before `actual_start`
    and the first reading after `actual_end` have the same odometer, the
    trip is 0 km (both ends = that value); otherwise it stays pending.
  - end below start: stays pending.
  - `km_start = round(start_m / 1000)`,
    `km_end = km_start + round((end_m - start_m) / 1000)` (half-up), so the
    trip distance is rounded once. `km_source = telematics`,
    `km_pending = false`.
- Cars without `tracker_imei`: `km_pending` is resolved manually by admin.
- Admin can enter km manually (`km_source = manual`) at any time; the job
  never touches a booking that is not `km_pending`. Return does not set
  `km_pending` when the admin already entered manual km during the trip.
- Admin edit of `actual_start`/`actual_end` on a booking with
  `km_source = telematics` sets it back to `km_pending` (km recomputed for
  the new window); manual km are kept.
- While any returned booking of a car with `tracker_imei` is `km_pending` for
  more than 24 h, all active admins get one mail per day listing them
  (tracker silent, offline or implausible data). The date of the last mail is
  persisted in `data/` so a restart does not resend it.
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

- Mandates are managed in the portal, independent of jVerein, in their own
  table with history (§4 mandate). The reference is `OCS-%06d` of the
  mandate id.
- Mandate is given **online** by the user under „Konto“: user enters account
  holder, IBAN and optional BIC, reads the mandate text, confirms. Stored as
  evidence: timestamp, client IP, mandate text version, creditor name and ID.
  (Online mandates are weaker evidence than paper; chargeback risk accepted
  by the Verein.)
- A new mandate ends every other `active` row of the user (`replaced`).
  Re-submitting identical holder, IBAN and BIC is idempotent: no new
  mandate, no mail. The POST response carries `"changed": true` (201, new
  mandate) or `false` (200, idempotent); the UI then says „Das Mandat ist
  unverändert.“
- The client echoes what it displayed: the POST body requires `text_version`
  and `creditor_id` (as returned by `GET /api/me/mandate`, `null` while not
  ready). If either differs from the current value (`creditor_id` compared
  after normalizing), the answer is 409 `text_changed` and nothing is stored;
  a missing `creditor_id` is 400 `invalid`.
- The mandate text is built into the server and versioned by a constant
  (`MANDATE_TEXT_VERSION`, currently 1; any text change bumps it). Creditor
  name and creditor ID come from the settings; users cannot give a mandate
  until both are set.
- After giving a mandate the user gets the mail „SEPA-Lastschriftmandat
  erteilt“ with reference, holder, masked IBAN and the mandate text. Mail
  failure does not undo the mandate.
- Revocation: user (`DELETE /api/me/mandate`, password required) or admin
  (`POST /api/admin/users/{id}/mandate/revoke`). It ends all `active` rows
  (`revoked`), cancels all `reserved` bookings of the user with cancel fee 0,
  deactivates the account and ends its sessions. An `active` (running)
  booking stays and is billed normally. The user path is refused while the
  user has an `active` booking (409 `trip_active`, „Bitte beende zuerst deine
  laufende Fahrt.“, nothing changed): a revoked user could never return the
  car. The admin path stays allowed (the admin can end the trip through the
  admin booking update). A wrong password on the user path is 400
  `bad_credentials` „Das Passwort ist falsch.“. The last active admin cannot revoke
  their own mandate (409 `last_admin`); an admin cannot revoke their own
  mandate through the admin endpoint (400 `self`).
- Booking gate: without a current mandate, `POST /api/bookings`,
  `PUT /api/bookings/{id}` and `POST /api/bookings/{id}/start` return
  403 `no_mandate`. Club trips are not exempt. Bookings created through the
  admin API are exempt from the create gate, but starting such a booking is
  gated like any other: a user without mandate cannot start it, and the admin
  sets `actual_start` by hand if needed. Admins are users and need a mandate to book
  themselves.
- Full IBANs never leave the server in API responses or mails; only
  `iban_masked` (`DE89 **** 3000`). Exception: the Verein's creditor IBAN is
  shown in full in the admin settings.
- Validation on input: IBAN restricted to SEPA countries (country length
  table, exact length, mod-97 == 1); BIC 8 or 11 characters; creditor ID
  mod-97 (DE: length 18). Characters other than `[A-Za-z0-9 ]` are invalid.
- Sequence type: always `RCUR` (allowed for the first collection since 2016).

### Invoice PDF
Minimal built-in PDF writer (standard Helvetica, WinAnsi encoding for
umlauts), one or more pages: association header, user address, invoice
number/date, period, one line per booking (date, times, hours day/night,
unused, km, fees), total, footer text (configurable), SEPA pre-notification
block.

## 10. Notifications (mail only)

Booking created / modified / cancelled, reminder before start (configurable,
default 1 h), late return reminder, invoice. Admin: billing run done,
km still pending 24 h after return (daily, §8).

## 11. Security

- Argon2id password hashing, session token (random 256 bit) in `HttpOnly`,
  `Secure`, `SameSite=Strict` cookie.
- Login rate limiting per IP and account.
- Password reset via mail link (single use, 1 h).
- All input parsed with length limits; parsers fuzzed (libFuzzer + ASan).
- `ocs-server` binds its HTTP port to localhost only; Caddy terminates TLS.
- Tracker port (§8) is the only port `ocs-server` exposes to the internet:
  plain TCP unless Teltonika TLS to an own server is verified and added.
  Mitigations: IMEI allowlist, limits, fuzzed parser, positions never stored,
  admin override of km.

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
GET/POST/DELETE /api/me/mandate   (DELETE needs the password)
GET  /api/cars
GET  /api/calendar?car=&from=&to=
GET  /api/bookings?scope=upcoming|history
POST /api/bookings                PUT /api/bookings/{id}     DELETE /api/bookings/{id}
POST /api/bookings/{id}/start     POST /api/bookings/{id}/return
POST /api/bookings/{id}/damage    GET /api/bookings/{id}/damage
POST /api/damage/{id}/photos      (raw image/jpeg body, ≤ 2 MiB)
GET  /api/damage/{id}/photos/{k}  (image/jpeg; owner or admin)
GET  /api/bookings/{id}/pin
admin:
  GET  /api/admin/users/{id}/mandates
  POST /api/admin/users/{id}/mandate/revoke
  (GET /api/admin/users includes a "mandate" summary per user)
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

### Phase 2: Trip lifecycle — done (merged 2026-11-01)
Plan: `docs/superpowers/plans/2026-10-31-phase2-trip-lifecycle.md`.
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
- Manual km entry by admin (`km_source = manual`); the telematics path comes
  in Phase 3, so `km_pending` is set on return and resolved manually for now.
- UI: start/return flow in "Meine Buchungen", no "Stornieren" on started
  bookings.
Done when: a booking can be started, returned, reported on and shows up in
history with actual times and km.

### Phase 3: Odometer (Teltonika telematics) — done (merged 2026-11-02)
Plan: `docs/superpowers/plans/2026-11-02-phase3-telematics.md`.
Depends on: Phase 2.
- Tracker ingestion (§8): TCP listener on `tracker_port`, Codec 8 Extended
  parser in `core/` (fuzzed), IMEI handshake against `tracker_imei`,
  `odometer_reading` store (append), 90-day pruning.
- `km_source` value `geotab` renamed to `telematics`; car admin field
  `tracker_imei`.
- Background job every 5 min resolves `km_pending` (§8); retries until found.
- Daily admin mail while a returned booking stays `km_pending` over 24 h.
- Tests with recorded/constructed Codec 8 Extended packets and a simulated
  tracker client; no hardware needed.
- Deployment docs: firewall port, Teltonika Configurator settings, SIM APN.
- Acceptance on hardware (separate, once bought): one tracker in the car for a
  week, km compared with the dashboard.
Done when: returned bookings get km automatically from tracker data, admin
can still override manually.

### Phase 4: Tariffs and pricing — done (merged 2026-11-02)
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
- Own mandate table with history (§4 mandate); the reference is derived from
  the id (`OCS-%06d`); evidence (timestamp, client IP, text version, creditor
  name and ID) is stored per row; changing IBAN/holder creates a new mandate
  and ends the old one.
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
