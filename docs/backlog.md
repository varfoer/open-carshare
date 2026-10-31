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
- [any] "Vereinsfahrt" badge in "Meine Buchungen" renders as plain grey text
  (`web/js/views/my_bookings.js`, `p.booking-car`); admin bookings shows a
  proper pill.

## Phase-bound

- [P2] Overdue `active` booking not shown as occupied in the user calendar:
  calendar window is `[max(from,now),to)` while occupation is `[start,now)`.
  Show an overdue `active` booking as occupied through now plus one slot.
- [P2] Free-check gate in `booking_rules.c` uses `old->state` instead of the
  new state.
- [P2] `active` booking started early can still have its club-trip flag
  changed by the user (check is `now >= booked_start`, should also cover
  `actual_start`).
- [P2] Web offers "Stornieren" for started bookings (server rejects).
- [P2] Test gaps in booking rules: `active` extension overlap, user/car
  change on update, unaligned update, admin update of `done`.
- [P7] Interim tar backup cron in `deploy/vps/README.md` is replaced by the
  encrypted mail backup.

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

## Deployment docs

- [any] README: first-install start before the unit exists; run
  `sudo -u ocs` after `cd /` when the repo is under a home dir; Caddyfile
  comment points to `/etc/default/caddy` (not read by the Debian unit);
  create-admin needs the server stopped.
