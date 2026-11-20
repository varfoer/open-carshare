# Car access (igloohome key box) — design

Date: 2026-11-20. Phase 9. Authority: `docs/spec.md` §4 (car, keysafe,
booking), §5 (start/return), §7 (car access), §11 (security), §12 (GDPR
export), §13 (API), §14/§15 (deferred, phases). This document changes the
spec where it says so; the plan updates the spec.

## Intent

Members get the car key without anyone handing it over. Each car has an
igloohome key box (Keybox 3 class, Bluetooth, no bridge) at its parking
place. When a member starts a trip, the app shows a PIN that opens the box;
the member takes the key, drives, and at return opens the box with the same
PIN, puts the key in and shuts it (the box locks by itself when shut).

The spec planned a self-computed PIN (`access_pin()` with a local secret,
"algorithm delivered later"). The bought hardware decides the algorithm:
igloohome PINs ("algoPIN") are computed by igloohome's cloud from a key only
igloohome and the box share; the box checks them offline. So the server
asks igloohome's API for a PIN instead of computing one.

Success: a member starts a booking at the car, sees a PIN, opens the box,
takes the key; at return the PIN still opens the box; a late or extended
trip gets a fresh PIN on demand; cars without a box work as today.

## Decisions by the product owner (2026-11-20)

- Use the igloohome API (cost accepted: about $2 per active lock per month
  after a 30-day trial; source: igloohome pricing page).
- The PIN is created when the trip starts, not ahead. Its start may lie up
  to one hour in the past.
- One PIN covers the whole trip (an hourly algoPIN), not one PIN per
  opening. Accepted consequences: algoPINs cannot be deleted remotely
  without an igloohome Bridge, so a PIN stays valid until its end even after
  an early return, and it can overlap the next member's booking by up to the
  rest of the booked time plus rounding.
- The box locks when shut; the PIN is needed at start (take key) and at
  return (put key back).
- After the return is recorded, the PIN is no longer shown; the return
  dialog shows the PIN so the member has it whatever order they do things
  in.
- A non-technical admin sets up a new box with a short manual: pair it in
  the igloohome app, then pick it in our car form and test it there. The
  igloohome API credentials stay in `ocs.conf`, entered once by whoever runs
  the server (option A; option B, credentials in the admin GUI, was
  rejected because it puts the secret into `data/` and every backup).

## Goals

1. Admin links a car to its box by picking the box by name from the boxes
   of the Verein's igloohome account, and checks it with a test PIN, all in
   the admin GUI.
2. Starting a booking of a car with a box creates an hourly algoPIN for the
   trip and shows it to the member on the booking card.
3. A member whose PIN has expired (late return, extension) or does not work
   gets a new PIN on request.
4. The return dialog shows the PIN with the instruction to put the key in
   and shut the box; after the return the PIN is hidden.
5. Cars without a box: start and return unchanged.

## Non-goals

- igloohome Bridge, remote unlock, remote PIN deletion, access logs from
  the box.
- One-time PINs, PINs created ahead of the start, PINs for admins (admins
  use the igloohome app on their own account).
- Bluetooth keys (they need the igloohome app; members use only the PWA).
- Any other key box vendor. The igloohome client sits behind one function so
  another vendor is a contained change later, but no abstraction layer is
  built for it now.
- ESP32 specifics.

## Anti-goals

- A member's PIN must never appear in the admin booking views, the GDPR export, logs,
  mails or the booking history.
- igloohome must never receive a member's name or email: the PIN's access
  name is `Buchung <id>`.
- A failed PIN request must never leave a booking `active` without a PIN
  (the member would be billed while standing at a locked box).
- The igloohome client secret must never be written to `data/`, logs or
  error responses.

## Facts the design rests on

From igloohome's developer pages, its docs (`docs.igloohome.co`), its
pricing page, the Keybox 3 manual and Seam's igloohome guide (read
2026-11-20):

- Auth: OAuth 2.0 client credentials. `POST https://auth.igloohome.co/oauth2/token`
  with `Authorization: Basic base64(client_id:client_secret)` and
  `grant_type=client_credentials`; answer `access_token`,
  `expires_in: 86400`, `token_type: Bearer`; no refresh token. Scopes
  include `igloohomeapi/algopin-hourly` and `igloohomeapi/get-devices`.
  After the trial, API calls answer 402.
- Devices: `GET https://api.igloohome.co/home/devices` (scope
  `get-devices`); response schema not documented.
- Hourly PIN (`docs.igloohome.co/home/api_access`, "Create an hourly
  AlgoPIN"): `POST https://api.igloohome.co/home/devices/{deviceId}/algopin/hourly`,
  headers `Authorization: Bearer <token>`, `Content-Type: application/json`,
  `Accept: application/json`. Body, all required:
  - `variance` integer, allowed values `1`, `2`, `3` (meaning not stated);
  - `startDate` `YYYY-MM-DDTHH:00:00+hh:mm`, hour resolution only, "may be
    up to 1 hour in the past" (also confirmed by the product owner);
  - `endDate` same format, at most `startDate` + 672 h; range 1 to 672 h;
  - `accessName` string (max length not stated).

  Answer 200 `{"pin": "5729314", "pinId": "AA9876B22D5E405C"}`. Documented
  errors: 400 (`{"error": "'accessName' is required"}`), 401 (invalid or
  expired token), 402 (`{"error": "30 days igloohome API trial has
  ended"}`), 403 (token scope lacks `igloohomeapi/algopin-hourly`), 404
  (`{"message": "not found device"}`), 409 (the device's property has no
  timezone: `{"message": "timezone is not found on property"}`), 415, 418
  (device type does not support AlgoPIN), 500. No rate limit and no
  algoPIN delete endpoint are documented.
- Seam's guide adds (stated as igloohome constraints, not in igloohome's
  docs): a PIN longer than 24 h must be used within 24 h of its start or it
  never works; at most 3 hourly PINs per hour (likely the three variances);
  at most 199 PINs stored per lock.
- algoPINs cannot be deleted through the API without a Bridge; deleting
  needs Bluetooth range.
- Berlin's UTC offset is always a whole number of hours, so full hours in
  UTC epoch seconds are full hours in local time.

## Approaches considered (how the server calls igloohome)

1. **Synchronous libcurl call inside the request handler (chosen).** The
   start and PIN requests call igloohome directly with a 5 s connect and
   10 s total timeout. The server stalls for about 1–2 s per call, at most
   10 s. Mail already does the same (synchronous libcurl in the event
   loop), so the code follows an existing pattern. Trade-off: other
   requests and tracker packets wait during the call; at 1–5 cars and a few
   starts per day that is acceptable.
2. **Asynchronous curl-multi inside the Mongoose loop.** No stall, but
   needs a pending-request state machine and deferred HTTP responses for
   little gain at this scale.
3. **Worker thread.** Breaks the single-threaded rule the booking checks
   rely on (spec §2).

The product owner left the how to the builder; option 1 was chosen.

## Design

### Data model (changes spec §4)

- **car**: new field `lock_device_id` (igloohome device ID; empty = no
  box). Optional, unique across cars (409 `duplicate_lock` „Diese
  Schlüsselbox ist bereits einem Auto zugeordnet.“). Format: 1..63 bytes of
  printable ASCII without spaces (exact igloohome format unknown; tighten
  once seen). Same handling as `tracker_imei` in the car form and codec
  (absent keeps the value, `""` clears it).
- **keysafe** entity, `car.keysafe_id`, `access_pin()` and the encrypted
  keysafe secret are removed from the spec. `/api/admin/keysafes` is
  dropped.
- **access_pin** (new table, `data/access_pins.jsonl`): one row per PIN
  created: `id`, `booking_id` (0 = admin test PIN), `car_id`, `pin_id` (igloohome), `pin`
  (digits; `""` once hidden), `valid_from`, `valid_until`, `variance`,
  `created_at`. Appended on creation. Kept out of `booking` so that
  `booking_to_json` (used by the GDPR export and admin views) cannot leak
  it. Rows older than 90 days are pruned with the existing prune timer.

### PIN window (pure, `core/`)

For a booking `b` at time `now`:

```
H(t)        = t - t mod 3600                       full hour at or before t
valid_from  = H(now)
valid_until = H(max(b.booked_end, now + 3600) + 3599)   full hour at or after
valid_until = min(valid_until, valid_from + 672 h)
```

Examples: start 14:20, booked end 17:45 → 14:00–18:00. Start 13:59, booked
end 14:00 → 13:00–15:00 (at least one hour ahead).

`H(now)` is at most 59:59 in the past, inside igloohome's "up to 1 hour in
the past" rule, so a PIN opens the box right after „Starten“.

A stored PIN is **usable** while `now < valid_until - 900` (15 min margin
so the member does not reach the box with a PIN about to expire).

### Start (changes spec §5, §7)

`POST /api/bookings/{id}/start` for a car with `lock_device_id`:

1. Existing checks (`br_check_start`, mandate).
2. Create a PIN (window above, variance below). On any failure (igloohome
   not configured, network, timeout, non-2xx, malformed answer): answer
   503 `pin_unavailable` „PIN für die Schlüsselbox konnte nicht abgerufen
   werden. Bitte erneut versuchen. Klappt es weiter nicht: Notfallkontakt
   auf der Hilfe-Seite.“ The booking stays `reserved`.
3. On success: append the `access_pin` row, then set the booking `active`
   as today. If storing fails after igloohome created the PIN, the PIN is
   lost (it stays valid on the box until its end; accepted).

Cars without `lock_device_id` start exactly as today.

### Showing and renewing the PIN (changes spec §13)

- `GET /api/bookings/{id}/pin` (owner only, booking `active`, car has a
  box; else 404): the newest PIN of the booking:
  `{pin, valid_from, valid_until}` while usable, else
  `{pin: null, valid_until}` (expired or none). Never calls igloohome.
- `POST /api/bookings/{id}/pin` `{renew: bool}` (same access rules):
  - `renew: false`: returns the usable PIN if there is one; otherwise
    creates a new one (window computed at `now`, so a late or extended trip
    is covered).
  - `renew: true` ("PIN funktioniert nicht?"): always creates a new one,
    but not within 1200 s of the booking's last PIN (429 `pin_too_soon`
    „Eine neue PIN ist ab <HH:MM> möglich.“); this keeps a booking under
    igloohome's 3 per hour.
  - Failure: 503 `pin_unavailable` as at start.
- A new PIN is created only when the member asks for one (or opens the
  return dialog with an expired PIN), never on extension and never in the
  background: a PIN longer than 24 h that is not used within 24 h of its
  start never works, so it should be created when the member is about to
  use it.
- Older PINs of the booking stay valid on the box until their end
  (same member; accepted).
- `GET /api/cars` adds `has_lock` (bool) so the web knows which cards need a
  PIN; the device ID itself stays admin-only.

### Variance

igloohome allows variance `1`..`3`. Each PIN request uses the next
variance for its car and `valid_from` hour, counting **attempts**, not
stored rows: a request that timed out or whose PIN could not be stored
still uses up its variance, so a retry never repeats a variance (builder's
call; the counter lives in memory, keyed by car and hour, and is seeded
from the stored rows after a restart). So a renewal in the same hour does
not return the same PIN again. This caps a car at 3 PIN attempts per start
hour (start plus two renewals, or two bookings of the same car starting in
the same hour plus one renewal); a fourth fails with `pin_unavailable`
„… Bitte ab <HH+1:00> erneut versuchen …“ (the next hour gives a new
`valid_from` and fresh variances). With the 1200 s renewal throttle a
single booking cannot exhaust its hour alone.

### Return (changes spec §5)

- Web: the return dialog shows the current PIN (fetching a new one with
  `POST … {renew:false}` if expired) above the damage step, with
  „Schlüssel in die Box legen und Box schließen.“, then the usual
  confirmation.
- Server: when a booking leaves `active` by any path (member return, admin
  edit that ends the trip), or an admin edit changes its `car_id` or
  `user_id`, the digits of all its `access_pin` rows are blanked (`pin =
  ""`, row rewritten). An admin edit that changes only times keeps the PIN
  (builder's call). `pin_id` and the window stay as a record.

### Web

- Booking card of an active booking whose car has a box: the PIN in large
  digits with „Gültig bis <HH:MM>“ (date if not today), a link „PIN
  funktioniert nicht?“ (`renew:true`). When expired: „PIN abgelaufen“ and a
  button „Neue PIN holen“ (`renew:false`).
- Start flow unchanged except that a 503 `pin_unavailable` keeps the
  booking reserved and shows the message.
- Help page: a section on the key box (PIN appears after „Starten“; put the
  key back before „Zurückgeben“; what to do when the PIN fails; emergency
  contact).
- Admin car form: see "Box setup in the admin GUI".

### Box setup in the admin GUI

The manual for the admin (German, section „Für Admins: Schlüsselbox
einrichten“ on the help page, linked from the car form):

1. In the igloohome app, logged in with the Verein's igloohome account:
   pair the box, put it in a property with time zone Europe/Berlin, name
   it (e.g. „Dörpsmobil 1“). This step cannot move into our GUI (Bluetooth
   pairing).
2. In the admin car form, choose the box under „Schlüsselbox“.
3. Press „Box prüfen“, enter the shown test PIN at the box: it opens.

Server and web:

- `GET /api/admin/locks` (admin): the boxes of the igloohome account as
  `[{device_id, name}]`, read through `GET /home/devices` (scope
  `get-devices`) on every call (no cache; builder may add a short one).
  Failure: 503 `locks_unavailable` with the cause in German (credentials
  missing or wrong, payment required, network). The response schema of
  `GET /home/devices` is not documented; the parser is written against the
  first real answer (verification item).
- Car form field „Schlüsselbox“: a select with „Keine“ plus the boxes by
  name. A car's current `lock_device_id` that is not in the list (or when
  the list fails) stays selected as „Unbekannte Box (<id>)“ so saving the
  form never drops it. When the list cannot be loaded, the field falls back
  to a text input for the device ID (builder's call).
- `POST /api/admin/cars/{id}/lock-test` (admin; car must have
  `lock_device_id`): creates a real hourly PIN for the current hour
  (`valid_from = H(now)`, `valid_until = valid_from + 1 h`, access name
  `Test Auto <car id>`), stores it as an `access_pin` row with
  `booking_id = 0` and `pin = ""` (the digits are only in the response;
  the row counts toward the car's variance in that hour), and answers
  `{pin, valid_until}`. Only the admin sees it, in the car form:
  „Test-PIN 5729314, gültig bis 15:00 — an der Box eingeben. Öffnet sie,
  ist alles eingerichtet.“ Errors map to plain German: credentials missing
  or rejected, payment required (402), box not found (404), box has no
  time zone (409, „In der igloohome-App der Box eine Zeitzone (Europe/
  Berlin) zuweisen.“), box type without PIN support (418), network. This
  is the only way an admin sees a PIN; it is the admin's own test, not a
  member's PIN (builder's call: an end-to-end test catches the time-zone
  and box problems that a device lookup cannot).

### igloohome client

- `core/igloo.c` (pure, unit-tested): build the PIN request body
  (`startDate`/`endDate` as ISO 8601 with the Berlin offset from
  `berlin_offset()`, e.g. `2026-11-20T14:00:00+01:00`; `variance`;
  `accessName` `Buchung <id>`), parse the token answer and the PIN answer
  (`pin` must be 4..12 ASCII digits, `pinId` 1..63 printable bytes; anything
  else is a failure).
- `platform_https_post()` in `platform/posix` (libcurl, TLS verification on,
  5 s connect, 10 s total, response capped at 16 KiB): returns HTTP status
  and body.
- Token cache in memory (`app_t`): fetched on first use, renewed 300 s
  before `expires_in` runs out; on a 401 from a PIN call, fetch a new token
  once and retry once.
- One overall budget of 15 s per PIN request across token fetch, PIN call
  and the 401 retry; a step that would start after the budget is spent
  fails with `pin_unavailable` (builder's call).
- Every failure answers the member with `pin_unavailable`; the log line
  names the cause so the admin can act: 402 „igloo: payment required“,
  403 „igloo: token scope lacks algopin-hourly“, 404 „igloo: unknown
  device <id>“, 409 „igloo: device property has no timezone“, 418 „igloo:
  device does not support AlgoPIN“, other statuses and curl errors with
  their code.
- No server-wide rate limiter (igloohome documents none); the variance
  cap above bounds attempts per car and hour.
- `app_t` gets a `pin_create` function pointer like `mail_send`, so API
  tests inject a fake and never call the network.
- Logging: one stderr line per failure with booking id, HTTP status or curl
  error; never the PIN, the token or the client secret.

### Configuration (spec §11, `ocs.conf`)

`igloo_client_id`, `igloo_client_secret` (from the igloohome API portal,
`web.igloohome.co/api`), `igloo_api_url` (base URL, default
`https://api.igloohome.co`), `igloo_token_url` (default
`https://auth.igloohome.co/oauth2/token`). Without `igloo_client_id`,
starting a car with a box fails with `pin_unavailable` (logged „igloo not
configured“); the server starts normally. Secrets stay in the config file,
not in `data/` (same rule as the SMTP password).

### Privacy and security (spec §11, §12)

- igloohome (Singapore) receives only the device ID, the window and
  `Buchung <id>`.
- PIN digits are stored in plain text in `data/access_pins.jsonl` while the
  trip runs (the backup is age-encrypted); they are blanked when the trip
  ends.
- The GDPR export and admin views do not include `access_pin` rows.
- The PIN endpoints are owner-only; an admin acting through the member
  endpoints is treated like any member.

## Side changes outside Phase 9 (hardware evaluation, 2026-11-20)

Separate small docs commit, before Phase 9:

- Spec §8: hardware is the Teltonika FMC003 (bought), not "e.g. FMC003";
  SIM Simbase with APN `simbase.eu` (pins the internet breakout to Europe;
  the tracker sends GPS positions over plain TCP, so their transit should
  stay in the EU).
- `deploy/vps/README.md` §10: APN `simbase.eu`.
- `docs/backlog.md`: retag `[P4] Tracker abuse` (Phase 4 is done) and add
  the option of a Simbase static public IPv4 for source-IP pinning
  (plan and cost unknown).
- `README.md`: "Not yet done" line updated.

## Spec changes (done by the Phase 9 plan)

§4 car (`lock_device_id`), keysafe removed, new access_pin; §5 start
(503 `pin_unavailable`); §7 rewritten from this design; §11 igloohome
call, config keys; §12 export excludes PINs; §13 `GET`/`POST
/api/bookings/{id}/pin`, `GET /api/admin/locks`, `POST
/api/admin/cars/{id}/lock-test`, `/keysafes` removed, `has_lock` in
`/api/cars`; §14 keysafe PIN algorithm removed from deferred; §15 Phase 9
entry. `deploy/vps/README.md`: new section "igloohome key box" for the
server operator (create the API client in the igloohome portal with scopes
`algopin-hourly` and `get-devices`, set the config keys, restart); the box
setup itself points to the admin manual on the help page.

## Verify during the hardware acceptance

None of these block planning; each has a known consequence.

1. The box's property in the igloohome app has time zone Europe/Berlin
   (else 409), and the PIN window matches local time (the DST switch can
   only be checked in March or October).
2. The response of `GET /home/devices`: field names for device ID and
   name, and whether it lists only boxes of the account (if it lacks a
   name, the select shows the ID).
3. Whether used or expired algoPINs count toward the box's 199 stored PINs.
   If they do, the box fills within one to two months; then
   `deploy/vps/README.md` gets a maintenance step (clear old PINs in the
   igloohome app over Bluetooth) and no code changes (the server cannot see
   a full box).

## Acceptance on hardware

With one box and the API trial: link the device ID to a car; start a
booking → PIN opens the box; return within the window → PIN opens the box;
a renewed PIN works; an expired PIN does not; with `igloo_client_id` unset,
starting a car with a box fails cleanly with `pin_unavailable`.

## Left to the builder

File and function names, where the "booking left active" hook lives (one
place every put path passes), the exact web layout of the PIN on the card,
test structure (fake `pin_create`, recorded igloohome answers for the
parser), help page wording.
