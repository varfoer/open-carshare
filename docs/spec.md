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
cancelled_at, club_trip (default false), invoice_id (0 = not invoiced, else
the number of the invoice that billed it), created_at, modified_at,
modified_by, start_reminded, late_reminded (reminder mail sent; default
false, §10).

### damage_report
id, booking_id, user_id, phase (`start`|`end`), text (required, < 1024 bytes),
n_photos (0..10), created_at. Photos are resized client-side to ≤1600 px JPEG
before upload; each upload is one raw `image/jpeg` body of at most 2 MiB
(2097152 bytes) starting with `FF D8 FF`, stored as
`data/photos/r<report id>-<k>.jpg` (k = 1..n_photos). "Keine Schäden" stores
nothing; only real damage creates a report.

### invoice
number (global gapless integer starting at 1, assigned at finalize, shown as
plain number: „Rechnung 17“; it is also the key, `booking.invoice_id` holds
it), user_id, run_id, period (YYYY-MM), total_cents (sum of the line
totals), n_lines, name, street, zip, city (addressee, snapshot at finalize),
created_at (finalize), issued_at (first send, 0 before; the invoice date is
its local date), mailed_at (0 until the mail went out), mandate_id (mandate
current at send; 0 for 0 EUR invoices and before send). Stored in
`data/invoices.jsonl`; a missing file means no invoices. The PDF is stored
at `data/invoices/<number>.pdf` (written at first send).

### invoice_line
One per billed booking, a snapshot of the computed figures at finalize:
invoice_number, booking_id, car_name, start, end (actual if started, else
booked; cancelled: booked), never_started, cancelled, club_trip, day_min,
night_min, unused_min, km, time_cents, km_cents, cap_cut_cents, late_fee_cents,
cancel_fee_cents, total_cents. Stored in `data/invoice_lines.jsonl`. Later
tariff, booking or user edits never change an issued invoice.

### billing_run
id, period (YYYY-MM), state (`draft`|`final`), created_at, finalized_at,
sent_at (0 until the first send), collection_date (`YYYY-MM-DD`, empty until
the first send), sepa_file (path relative to `data/`, empty if none).
Stored in `data/billing_runs.jsonl`; the SEPA XML is
`data/billing/run-<id>-pain008.xml`. A draft holds no invoices: it is
computed live from the bookings on every read. Loading drops invoices and
lines of a run that is still `draft` (crash safety).

### settings
String keys `assoc_name`, `assoc_street`, `assoc_zip`, `assoc_city`,
`creditor_id` (Gläubiger-ID), `creditor_iban`, `creditor_bic` (the admin
settings PUT requires all keys), start_tolerance_min (15), cancel_deadline_min (0), cancel_fee_cents (0),
`sepa_lead_days` (integer 5..30, default 5: days between the send and the
collection; the minimum is the 5-day pre-notification promised in the
mandate text), `invoice_footer` (free text, printed on the invoice PDF; it must fit the
PDF footer, at most 24 lines, else the settings PUT answers 400 `invalid`),
`reminder_before_min` (integer 0..1440, default 60, 0 = no start reminder;
required in the settings PUT), SMTP config.

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

A run is created for a closed month (`period_end <= now`, else 400
`period_open` „Der Monat ist noch nicht abgeschlossen.“). Runs are
ascending: no new run while a draft is open (409 `draft_exists`), and the
period must be greater than the period of every existing run (409
`period_exists`). A draft can be deleted (`DELETE`); a final run cannot (409
`not_draft`).

1. **Inclusion.** A booking is billed in the first run whose period end is
   after its bill time: `invoice_id == 0` and bill time `< period_end`. Bill
   time is `actual_end`, or `booked_end` if never started, or `cancelled_at`
   for a cancelled booking. A booking whose times an admin moves into an
   already billed month therefore lands in the next run instead of being
   lost. Cancelled bookings are included only with `cancel_fee_cents > 0`
   and not `club_trip`; `reserved` and `active` bookings are not.
2. **Draft.** Per user, line items and totals computed live from the current
   bookings (every read recomputes; reservations past their end are expired
   first). Users are sorted by name (tie: user id), lines by start (tie:
   booking id). Prices come only from the pricing engine (§6); the invoice
   total is the sum of the line totals. The admin reviews and may edit
   bookings; the next read shows the result. The draft lists blockers:
   `km_pending` (every included booking with km pending, club trips
   included), `no_tariff` (included non-cancelled non-club bookings without a
   tariff at `booked_start`), `no_mandate` (users with total > 0 without a
   current mandate).
3. **Finalize** (`POST …/finalize`). Refused with 409 `no_creditor` „Bitte
   zuerst Vereins- und Gläubigerdaten eintragen.“ while association name,
   address, creditor ID or creditor IBAN are empty, and with 409 `blocked`
   „Abschluss nicht möglich.“ (response carries `blockers`) while any
   blocker is non-empty. Otherwise the figures are computed again from the
   state at that moment (not what the admin saw), invoice numbers are
   assigned in user order (gapless), invoices and lines are stored, bookings
   are locked (`invoice_id` set) and the run becomes `final`, in one
   persisted step: a crash leaves no numbers burned and no locked bookings.
   The invoice stores the addressee as it is at that moment. No PDF, no
   collection date, no XML yet.
4. **Send** (`POST …/send`, only for a `final` run, else 409 `not_final`
   „Der Abrechnungslauf ist noch nicht abgeschlossen.“). The first send
   issues everything: it refuses with 409 `blocked` „Versand nicht möglich.“
   while a user with total >
   0 has no current mandate (the mandate current at send is used and stored
   as `invoice.mandate_id`), then sets the collection date, the invoice date
   (`issued_at`), writes all invoice PDFs and the SEPA XML, and records
   `sent_at`. Collection date = first TARGET2 business day on or after
   (local send date + `sepa_lead_days`); TARGET2 closing days are Saturday,
   Sunday, 1 Jan, Good Friday, Easter Monday, 1 May, 25 Dec, 26 Dec. The
   mail is the SEPA pre-notification (amount, collection date, mandate ref,
   creditor ID), so the date is fixed here, not at finalize. Then every
   invoice with `mailed_at == 0` is mailed (PDF attached; the user is
   addressed with „du“) and `mailed_at` is persisted right after each
   successful mail (a failed persist stops the loop with 500, so no user is
   mailed twice). The answer is
   `{"sent": n, "run": …, "failed": [invoice numbers]}`. Send is
   retryable: a repeated call mails only the unmailed invoices; collection
   date, PDFs and XML of the first send stay. A retry with at least one
   unmailed invoice with total > 0 is refused with 409 `prenotify_late`
   „Der Einzugstermin ist zu nah, die Vorabankündigung wäre nicht mehr
   fristgerecht.“ (nothing is mailed) when the local date of now +
   5 days (`SEPA_PRENOTIFY_MIN_DAYS`) is later than the stored collection
   date.
5. Admin downloads the SEPA XML (`GET …/sepa.xml`, „SEPA-XML herunterladen“,
   404 while it does not exist or if the run has no entry with total > 0)
   and imports it into Hibiscus. Invoice PDFs are downloadable per invoice
   after the first send.
6. **Edit lock.** Admin edits (`PUT`/`DELETE /api/admin/bookings/{id}`) of a
   booking with `invoice_id != 0` are refused with 409 `invoiced` „Die
   Buchung ist bereits abgerechnet.“

A club trip appears on the invoice as a line with date/times and km (from
the booking), amount 0 EUR, marked "Vereinsfahrt". A user whose month has
only club trips gets a 0 EUR invoice listing them: it takes the next invoice
number and is mailed like any other, but has no SEPA entry, the mail carries
no pre-notification, and it needs no mandate. Users with no invoice lines at
all get no invoice. Users with a total above 0 and without a valid mandate
block finalization (listed in the draft) and the first send.

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
  (`MANDATE_TEXT_VERSION`, currently 2; any text change bumps it). The text
  names the creditor (name, address, creditor ID) and promises a
  pre-notification of 5 calendar days (`SEPA_PRENOTIFY_MIN_DAYS`). Creditor
  name, address and creditor ID come from the settings; users cannot give a
  mandate until name, street, ZIP, city and creditor ID are set.
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
  IBANs of non-EEA countries (`AD CH GB GI MC SM VA`) require a BIC: 400
  `bic_required` „Für diese IBAN ist die BIC erforderlich.“.
- Sequence type: always `RCUR` (allowed for the first collection since 2016).
- `pain.008.001.02` details: scheme `CORE`, `SeqTp` `RCUR`, `ChrgBr` `SLEV`,
  `BtchBookg` `true`; one transaction per invoice with total > 0, end-to-end
  ID `OCS-RE-<invoice number>`, remittance „Rechnung <n> Carsharing
  <Monat Jahr>“; message ID `OCS-RUN-<run id>`. Text fields are restricted
  to the SEPA set `A-Z a-z 0-9 / - ? : ( ) . , ' +` and space (names max 70,
  remittance max 140) by transliteration: ä→ae, ö→oe, ü→ue, ß→ss, `&`→`+`
  („Jürgen Müller-Weiß & Söhne“ becomes `Juergen Mueller-Weiss + Soehne`).
  An empty creditor or debtor BIC is written as `<Othr><Id>NOTPROVIDED</Id>
  </Othr>`. The full IBANs appear only in the XML file, never in API
  responses or mails. The schema is vendored at
  `server/test/data/pain.008.001.02.xsd` for the `pain008_xsd` test.

### Invoice PDF
Minimal built-in PDF writer (standard Helvetica, WinAnsi encoding for
umlauts), one or more pages: association header, user address, invoice
number/date, period, one line per booking (date, times, hours day/night,
unused, km, fees), total, footer text (`invoice_footer`), SEPA
pre-notification block (mandate reference, masked IBAN, collection date; only
for total > 0). Stored under `data/invoices/<number>.pdf`; written at the
first send, so the admin cannot download a PDF before that. The mail carries
it as attachment `Rechnung-<number>.pdf`.

## 10. Notifications (mail only)

Booking created / modified / cancelled, reminder before start (configurable,
default 1 h), late return reminder, invoice. Admin: billing run done,
km still pending 24 h after return (daily, §8).

Mails address the user with „du“; times are Europe/Berlin, `04.11.2026 14:30`.
A failed mail never changes the HTTP response of the request that triggered
it (the mailer logs it).

- **Recipient** of every booking and reminder mail is the booking's user, not
  the actor: an admin creating, editing or cancelling a member's booking mails
  the member (the text then says the Vereinsverwaltung made the change).
  Inactive users get no booking or reminder mail.
- **Created** (`Buchung <id> bestätigt`): user `POST /api/bookings` and admin
  `POST /api/admin/bookings`.
- **Modified** (`Buchung <id> geändert`): user or admin `PUT` that changes
  `booked_start`, `booked_end`, `car_id` or `club_trip`. Admin edits of only
  actual times or km send nothing. An admin `PUT` that moves the booking to
  another member mails „storniert“ to the old and „bestätigt“ to the new
  member and re-arms both reminders.
- **Cancelled** (`Buchung <id> storniert`, with the cancel fee if > 0): user
  or admin `DELETE`. Start, return, expiry and the cancellations caused by
  mandate revocation or deactivation send nothing.
- **Start reminder** (`Erinnerung: Fahrt ab HH:MM`): once for a `reserved`
  booking when `booked_start - reminder_before_min <= now < booked_start`,
  unless the booking was created inside that window. Off when
  `reminder_before_min` is 0.
- **Late-return reminder** (`Rückgabe überfällig: <car>`): once for an
  `active` booking when `now >= booked_end`. It names the late fee only when
  the tariff at `booked_start` has `late_fee_cents_per_started_h > 0` and the
  booking is not a club trip.
- **Once only:** `start_reminded` / `late_reminded` are set only after a
  successful send, so a failed send is retried on the next 5-min tick while
  the condition holds. A change of `booked_start` clears `start_reminded`, a
  change of `booked_end` clears `late_reminded`. The reminders run in the
  5-min expiry job, after expiry; the first failed send ends that tick.
- Mail is sent synchronously in the event loop: while the SMTP relay is
  unreachable, a booking request that mails waits up to the SMTP timeout
  (connect 5 s, total 20 s) before it answers.
- **Trip finished** (`Fahrt beendet: <car>`, to every active admin, so they
  can check and later unplug the car at the public charger): when a member
  returns a trip (`POST /api/bookings/{id}/return`). Body: the local return
  time (`Zurückgegeben: 04.11.2026 14:30`) and a link to `#/admin/buchungen`.
  An admin closing a forgotten trip and an expired reservation send nothing.
- **Billing run done** (`Abrechnung <Monat Jahr> versendet`, to every active
  admin): after each send call that reached the mail loop, retries included;
  not when the send was refused. Content: run id, period, invoices mailed so
  far / total, mailed by this call, failed invoice numbers, run total,
  collection date, link to `#/admin/abrechnung`.

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
- Nightly backup: `deploy/vps/ocs-backup.sh` (bash, systemd timer 03:15, no
  crypto in C). `ocs-data-YYYY-MM-DD.tar.gz.age` = `data/` without `photos/`
  and `*.tmp`, encrypted with age to `backup_recipient` (the admin's public
  key; the private key never goes onto the server); on Sundays (or with
  `--photos`) also `ocs-photos-YYYY-MM-DD.tar.age`. tar is piped into age:
  the plaintext never touches the disk and never travels by mail (the backup
  contains IBANs and password hashes). Both are kept in `backup_dir` (mode
  0600) for 30 days and mailed to `backup_mail_to` via the configured SMTP
  relay; a file over `backup_max_mail_bytes` (default 20 MB) is not
  attached, the mail says so. GNU tar exit 1 ("file changed as we read it")
  counts as success.
- Config keys `backup_recipient` (`age1…`), `backup_mail_to`, `backup_dir`
  (default `/var/backups/ocs`), `backup_max_mail_bytes` (1..100000000) live in
  `ocs.conf`; the server validates them and does not use them otherwise.
- Restore: `ocs-restore.sh <archive.age> <identity> <target>` into an empty
  directory (refuses a non-empty one), then `ocs-server -c <conf> check-data`
  loads the data dir and prints table counts (exit 0 ok, 1 load failure);
  it never writes a table.
- GDPR export: `GET /api/admin/users/{id}/export` returns `exported_at`,
  `user` (public, no `pw_hash`), `bookings`, `damage_reports` (each with
  `photos`: API paths `/api/damage/{id}/photos/{k}`), `mandates` (masked
  IBAN only, `iban_masked`), `invoices` (each with its `lines`). Sessions,
  tokens and odometer readings are not exported. Full IBANs never leave the
  server, the export included. The web saves it as `ocs-export-<id>.json`.
- Deactivation (admin user PUT with `active:false`) deletes the sessions and
  cancels every `reserved` booking of the user with cancel fee 0, like a
  mandate revocation (a deactivated user cannot start it, and expired it
  would be billed as unused time). An `active` booking stays and is billed.
- Invoice retention is a guarantee, not a job: nothing deletes invoices,
  invoice lines, invoice PDFs, mandates or invoiced bookings; deactivated
  users keep their records for the 10-year retention. There is no automatic
  purge after that period.

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
  GET  /api/admin/users/{id}/export      (GDPR export, §12)
  POST /api/admin/users/{id}/mandate/revoke
  (GET /api/admin/users includes a "mandate" summary per user)
  /api/admin/users, /cars, /keysafes, /tariffs, /bookings, /settings
  GET  /api/admin/billing                       (runs, newest first, with n_invoices, total_cents)
  POST /api/admin/billing?period=YYYY-MM        (new draft)
  GET  /api/admin/billing/{id}                  (detail; draft: live, with blockers)
  DELETE /api/admin/billing/{id}                (draft only)
  POST /api/admin/billing/{id}/finalize | /send  (409 blocked: finalize „Abschluss nicht möglich.“,
                                                  send „Versand nicht möglich.“; send also 409 prenotify_late)
  GET  /api/admin/billing/{id}/sepa.xml         (application/xml)
  GET  /api/admin/invoices/{number}/pdf         (application/pdf, after first send)
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

### Phase 5: SEPA mandates — done (merged 2026-11-03)
Depends on: Phase 1.
- User enters account holder, IBAN (mod-97), optional BIC; reads the
  versioned mandate text and confirms online (§9 SEPA).
- Own mandate table with history (§4 mandate); the reference is derived from
  the id (`OCS-%06d`); evidence (timestamp, client IP, text version, creditor
  name and ID) is stored per row; changing IBAN/holder creates a new mandate
  and ends the old one.
- Admin: mandate status per user; settings for creditor ID, IBAN, BIC.
Done when: users can give and replace a mandate and admin sees who lacks one.

### Phase 6: Billing run — done (merged 2026-11-03)
Plan: `docs/superpowers/plans/2026-11-03-phase6-billing-run.md`.
Depends on: Phases 4 and 5 (Phase 3 for the `km_pending` gate).
- Billing run per month (§9): draft, admin edits and recompute, finalize
  (gapless invoice numbers, bookings locked via `invoice_id`), send.
- Finalize blocked by `km_pending` and by users with total > 0 without a
  valid mandate; 0 EUR invoices for months with only club trips.
- Invoice PDF writer (§9 Invoice PDF), invoice mail with SEPA
  pre-notification.
- SEPA `pain.008.001.02` export for Hibiscus, `sepa_lead_days` setting.
Done when: a month can be billed end-to-end and the XML imports into
Hibiscus.

### Phase 7: Notifications and operations — done (merged 2026-11-03)
Plan: `docs/superpowers/plans/2026-11-03-phase7-notifications-ops.md`.
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
