# Backlog

Known defects and deferred review findings, carried over from Phase 1 and
club trips. Tag `[Pn]` means the item belongs to that phase (spec §15);
`[any]` means fix opportunistically when touching the area. Remove an item
when it is fixed.

## Open bugs

- [any] `POST` without body and without `Content-Length` (e.g. `curl -X POST`
  on `/api/admin/users/{id}/invite`) is rejected with 413 `too_large`.
  Browsers send `Content-Length: 0` and are unaffected. Look at the body
  length check in `server/src/http/server.c` `on_event`.

## Phase-bound

- [any] Tracker abuse with a known IMEI:
  the IMEI is printed on the device, so anyone with it can send readings:
  fake trip-end km within the 90-day window, unbounded readings growth,
  event-loop stall via many packets (fsync + O(n) duplicate scan). Also
  hold the 16 connection slots by reconnecting every 30 s; half-open
  connections linger up to 900 s. Decide: per-connection packet rate limit,
  readings cap per car per day, one live connection per IMEI (close older
  on handshake), plausibility check at resolve (km/h), TLS or source-IP
  pinning once firmware is verified. Simbase offers a static public IPv4
  (plan and cost unknown), which would allow source-IP pinning.

## Server

- [any] db contract: copy before modify (`user_t u = *db_user(..)`), never pass
  a pointer into the db to `db_put_*`; pointers into a table are invalid
  after any put to it.
- [any] Rate limit: when all 256 slots are blocking, new keys get 429 for up
  to 15 min (needs ~256 distinct client IPs; accepted).
- [any] `PUT /api/me/password` not rate limited.
- [any] Password reset burns the token before hash/store succeeds
  (`api_auth.c`).
- [any] Invite allowed for inactive users; user create with token store
  failure returns 500, retry gives 409 (recover via invite).
- [any] Static `sort_db` global used with `qsort` in `api_booking.c` and
  `api_admin.c` (single-thread assumption).
- [any] Codec: out-of-range enum serialized as `names[0]`; cJSON OOM in
  `X_to_json` ignored; non-object input leaves `*out` untouched.
- [any] `db_prune` not atomic across sessions and tokens tables; dir-fsync
  error in `store.c` not logged.
- [any] Function-pointer casts in `db.c` are UB under
  `-fsanitize=function`.
- [any] Mail: bare CR in body emitted raw; long subjects not folded;
  `url_is_local` fooled by `smtp://evil.com?@localhost` (config only);
  NULL message crashes `platform_mail_send`.
- [any] HTTP: cookie lookup via substring match; X-Forwarded-For trusted only
  from IPv4 `127.0.0.1`; chunked bodies over 64 KiB buffered up to 3 MB
  before 413; 301 header truncates for raw URI over ~2 KB; `//sub` yields a
  protocol-relative `Location` when the directory exists; dotfiles served
  from `web_dir`.
- [any] `main.c`: password buffer not wiped on error paths;
  `curl_global_cleanup` never called; config file permissions unchecked.
- [any] Build: test sources and fuzz harness not compiled with
  `-Wall -Wextra -Werror`; `ocs_core` exposes the mongoose include dir
  publicly; GET routes and query strings not fuzzed.
- [any] Test gaps: argon2id known-answer vector, store failure paths
  (`delete_sessions`, `prune`, `put_settings`), PUT path of
  `admin_lacks_club`.
- [any] Damage: `damage_report_to_json` non-NUL text branch untested;
  `db_load` fails if `photos/` cannot be created (read-only data dir).
- [any] Booking rules test gaps: `now == booked_end` boundary
  (`occupied_end`, `BR_EXPIRED`); `DMG_START` on ACTIVE with
  `actual_start == 0`.
- [any] Overdue grace (`now + 1800`) blocks extension of the next
  reservation and unrelated admin edits of either booking with 409 overlap
  while the car is overdue (closing the trip still works).
- [any] Trip API: `test_start_blocked_409_car_busy` relies on early start;
  `?scope` without `=` treated as upcoming; `api_expire_bookings` partial
  progress on storage failure undocumented; expiry rewrites `bookings.jsonl`
  once per expired booking (O(n²) on first sweep).
- [any] Admin trip edit: clearing/reopening actuals keeps km (a "Nicht
  angetreten" card can show manual km); combined `actual_end` + km and
  non-integer `actual_*` PUT untested.
- [any] Photo files mode 0644 inside 0700 `photos/`; `fuzz_api.c` literal
  65536; binary reply reason phrase only OK/Error; stale "Tasks 8/9: add
  rows here" comment above `ROUTES` in `api.c`.
- [any] Damage reports unlimited per booking (10 × 2 MiB each, anytime);
  unauthenticated clients can make the server buffer 2 MiB on photo routes
  (check the session cookie at HDRS for `BODY_JPEG` routes).

## Web

- [any] Calendar resize listener never removed (extra fetches after
  navigation); two bookings in one 30-min slot hide the second label.
- [any] Booking form: no client-side lower bound for an earlier start; silent
  return on empty date; DST-gap time shifted without hint.
- [any] Admin: license date cannot be cleared; stale-response race in admin
  bookings `load()`; inactive users unmarked in the booking user dropdown;
  car "Deaktivieren" without confirmation; admin cannot create bookings in
  the UI; cancel uses `window.confirm`.
- [any] Password reset token stays in history; `parseHash` drops text after a
  second `?`.
- [any] Service worker: `/api` bypass misses exact `/api`; background
  `cache.put` not in `waitUntil`; `skipWaiting` + `claim` can mix old page
  and new views after deploy; `addAll` may precache HTTP-cached files (use
  `cache: 'reload'`); no check that the shell list matches `git ls-files web`.
- [any] Tests: no guard that device TZ differs from Berlin; no year-boundary
  or midnight cases; no DOM test of form_time helpers.
- [any] My bookings: "since" paragraph built for every card; "Starten"
  offered days ahead with only the damage dialog as confirmation (show the
  booked start in the dialog title when early).
- [any] Admin bookings: `showDamage` stale-fetch race and error `<p>` not
  `messageBox`; clearing both km fields sends nothing; time select keeps the
  old value after "leeren".

## Deployment docs

- [any] README: first-install start before the unit exists; run
  `sudo -u ocs` after `cd /` when the repo is under a home dir; Caddyfile
  comment points to `/etc/default/caddy` (not read by the Debian unit);
  create-admin needs the server stopped.

## SEPA

- [any] `DELETE /api/me/mandate` and `PUT /api/me/password` are not rate
  limited (password guessing for a logged-in session).
- [any] A failed user revoke is not retryable if a late step fails: move the
  mandate step last in `api_mandate_revoke`.

## Billing

- [any] Run `pain008_xsd` with `xmllint` in CI/dev: `xmllint` is not
  installed on the dev machine, so the test skips. The vendored schema
  (`server/test/data/pain.008.001.02.xsd`, from the python-sepaxml mirror)
  was checked once against the sample XML with python `xmlschema`.
- [any] Name sort in the billing run uses `strcmp`: Ä, Ö, Ü sort after Z.
  Use German collation (or fold umlauts for the sort key).
- [any] Invoice footer is printed on the last PDF page only.
- [any] PDF WinAnsi mapping covers only € „ “ – ’ ” … —; any other
  character outside Latin-1 prints `?`.
- [any] PDF width tables come from the gsfonts AFMs and differ from Adobe
  Core14 for about 10 rare glyphs: regenerate from the urw-base35 NimbusSans
  AFMs.
- [any] One mandate revoked between finalize and send blocks sending the
  whole run (no per-user exclusion).
- [any] `billing_line` writes `end = 0` for a started booking with
  `actual_end == 0` (bill time falls back to `booked_end`).
- [any] Admin cannot preview invoice PDFs before send (PDFs are written at
  the first send).
