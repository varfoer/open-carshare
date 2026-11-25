# open-carshare

> [!CAUTION]
> Completely AI-generated code. Currently untested.

Booking portal for a small carsharing association (Verein): 1–5 cars,
20–50 members. Members book cars in a calendar, start and return trips on
their phone, and pay by SEPA direct debit. Admins manage members, cars,
tariffs and the monthly billing run.

UI language: German. Code and docs: English. License: GPL-2.0-only
(see `LICENSE`).

## Features

- Booking calendar per car, booking rules, cancellations, reminders by mail.
- Trip start and return with damage reports (photos).
- Odometer via Teltonika OBD tracker (optional), else manual km entry.
- Tariffs (day/night rates, caps, late fees) and price estimate.
- SEPA mandates given online; monthly billing run with invoice PDFs by mail
  and a `pain.008` XML file for import into the bank software (Hibiscus).
- Admin views: users (invite by mail), cars, bookings, billing, settings.
- Installable as a PWA (no app store), dark mode, help page
  (`web/help.html`).
- Car key in an igloohome key box, PIN shown at trip start.
- Mail notifications; reminders (start, end of booking, late return) as
  Web Push on devices that turned it on.

Not yet done: ESP32 port.
See `docs/spec.md` §14.

## Architecture

```
Browser / PWA  --HTTPS-->  Caddy (TLS)  --HTTP localhost-->  ocs-server (C)
                                                              |-- data/  (JSON-lines files)
                                                              |-- SMTP relay
Teltonika trackers  --TCP (Codec 8 Extended)-->  ocs-server tracker port
```

- `ocs-server`: single C binary, single-threaded event loop, all data in
  RAM, persisted as JSON-lines files. No database.
- `web/`: vanilla HTML/JS/CSS, no build step, served by `ocs-server`.
- Libraries (vendored in `server/third_party/`): Mongoose, cJSON,
  Monocypher, Unity; libcurl for SMTP and HTTPS, OpenSSL libcrypto for Web
  Push.

| Path | Content |
|---|---|
| `server/src/core/` | pure logic (booking rules, pricing, billing, SEPA, PDF), no I/O |
| `server/src/http/` | API handlers, static files, tracker listener |
| `server/src/store/`, `auth/`, `platform/` | file store, passwords/sessions, OS layer |
| `server/test/`, `server/fuzz/` | Unity tests, libFuzzer targets |
| `web/` | client; `web/test/` = Node tests |
| `deploy/vps/` | systemd units, Caddyfile, config example, backup scripts |
| `docs/spec.md` | full specification (data model, rules, API) |
| `docs/backlog.md` | known open issues |

## Development

Requirements: `cmake` (>= 3.22), a C11 compiler, `libcurl4-openssl-dev`,
`libssl-dev` (OpenSSL >= 3.0),
Node.js (web tests only).

```sh
cmake -B build                    # debug build with ASan/UBSan (default)
cmake --build build -j
ctest --test-dir build --output-on-failure
node --test web/test/
```

Fuzzing (clang):

```sh
CC=clang cmake -B build-fuzz -DOCS_FUZZ=ON
cmake --build build-fuzz -j
./build-fuzz/server/fuzz_teltonika -max_total_time=60
./build-fuzz/server/fuzz_api -max_total_time=60
```

Run locally. Minimal config (`data_dir`, `web_dir`, `base_url` are
required; without SMTP keys no mail goes out):

```sh
mkdir -p /tmp/ocs/data
cat > /tmp/ocs/ocs.conf <<EOF
listen = http://127.0.0.1:8080
data_dir = /tmp/ocs/data
web_dir = $PWD/web
base_url = http://localhost:8080
EOF
read -rs PW && printf '%s\n' "$PW" | ./build/server/ocs-server \
  -c /tmp/ocs/ocs.conf create-admin admin@example.org "Admin"; unset PW
./build/server/ocs-server -c /tmp/ocs/ocs.conf
```

Open <http://localhost:8080>. Passwords need at least 10 characters.

Command line: `ocs-server -c <conf> [create-admin <email> <name> | check-data]`.

## Deployment

Target: one Debian/Ubuntu VPS, `ocs-server` on `127.0.0.1:8080` behind
Caddy (Let's Encrypt). Step-by-step guide: `deploy/vps/README.md`.

1. Install packages, build a release binary
   (`cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DOCS_SANITIZE=OFF`).
2. Create user `ocs`, install binary to `/opt/ocs/`, `web/` to
   `/opt/ocs/web/` (without `web/test/`).
3. Copy `deploy/vps/ocs.conf.example` to `/etc/ocs/ocs.conf`; set
   `base_url` and SMTP relay.
4. Create the first admin (`create-admin`, service stopped).
5. Install the systemd unit and Caddyfile, open ports 80/443 (and the
   tracker port if used).
6. Optional: nightly encrypted backup (`age`), Teltonika tracker.

After the first login the admin enters association data, creditor ID and
creditor IBAN under „Einstellungen“. Every member gives a SEPA mandate under
„Konto“ before booking.

Updating: see `deploy/vps/README.md` „Updating“. For every web change bump
`CACHE` in `web/sw.js`, or installed PWAs keep the old files.

## Usage

- **Members** are created by an admin and get an invite mail (link valid
  7 days) to set their password. They install the app from the browser
  (Android: „App installieren“, iOS Safari: „Zum Home-Bildschirm“).
- **Book** in „Kalender“; start the trip in „Meine Buchungen“, report
  damage, return the car. Km come from the tracker or are entered by hand.
- **Billing** (admin, monthly): „Abrechnung“ → create run, check blockers,
  finalize, send (invoice mails with PDF), download the SEPA XML and import
  it into Hibiscus.
- **Help**: `web/help.html` holds contact data and manuals; edit it in the
  repo (contact data are placeholders until filled in).
