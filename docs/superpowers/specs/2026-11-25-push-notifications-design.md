# Push notifications and end-of-booking reminder

Status: approved by the product owner on 2026-11-25 ("Your recommendations sound good. implement that way.")

## Why

Field test (2026-11-23/24) with real hardware: members want a quick reminder that their booking ends
soon, so they return on time (late fees, next member waiting). A push message on the phone reaches them
while driving, where a mail goes unnoticed.

## Goals

- New **end reminder** shortly before `booked_end` of an active trip (setting, default 30 min).
- **Web Push** as a channel for the three reminders: start, end (new), late return.
- A member with push enabled on at least one device gets these reminders as push **instead of** mail.
  Without push (or when no push is delivered) the reminder goes by mail, as today. Exception (product
  owner, 2026-11-25, after review): the late-return reminder always also goes by mail.
- Opt-in per device under „Konto“, with a test button.

## Non-goals

- Booking confirmations, changes, cancellations, invoices, admin mails: stay mail only.
- No push for admins' "trip finished" or billing mails.
- No native app, no vendor push account (Firebase project etc.): standard Web Push with VAPID only.
- No per-event opt-out; push on = all three reminders by push.

## Anti-goals

- A reminder must not get lost: if no push is delivered, mail is sent in the same tick.
- No plaintext reminder content at Google/Apple/Mozilla: payload encrypted end-to-end (RFC 8291).
- The server must not POST to arbitrary URLs supplied by members (SSRF): endpoint host allow-list.

## Approaches

1. **Web Push, push replaces mail for reminders, mail fallback** (chosen). One channel per reminder,
   no duplicates; mail stays the safety net.
2. Push in addition to mail for every reminder: duplicates, members learn to ignore one of them.
3. Push for the end reminder only: inconsistent; start and late reminders are just as time-critical.

## Constraints and platform facts

- Android (Chrome, also other Chromium browsers and Firefox): works in browser and installed PWA.
- iPhone/iPad: only for the app installed to the home screen, iOS/iPadOS 16.4+.
  In Safari tabs `PushManager` is missing; the UI says to install the app first.
- Each device asks for notification permission once (user gesture: button click).
- Push requires ES256 (VAPID, RFC 8292) and aes128gcm payload encryption (RFC 8291/8188):
  P-256 ECDH/ECDSA, HKDF-SHA-256, AES-128-GCM. Monocypher has none of these, so the server links
  OpenSSL libcrypto (already present as dependency of `libcurl4-openssl-dev` / `libcurl4`). The
  primitives sit behind `platform.h` so an ESP32 port can use mbedTLS.
- Sends are synchronous libcurl calls in the event loop, like SMTP and igloohome.

## Design

### Data

- `push_subscriptions.jsonl` (new table): `id, user_id, endpoint, p256dh, auth, created_at`.
  `endpoint` unique (re-registering moves it to the current user); at most 5 per user, the oldest is
  dropped on the 6th. `p256dh`/`auth` base64url as sent by the browser (65 / 16 bytes decoded).
- `vapid.json` in the data dir: `{"private":"<base64url 32 bytes>"}`; generated at first start (mode
  0600); the public key is derived. Part of the normal backup. If it is lost, browsers re-subscribe on
  the next app start (client compares keys).
- `booking.end_reminded` (bool, default false), like `start_reminded` / `late_reminded`.
- Setting `end_reminder_before_min` (0..1440, default 30, 0 = off).

### Endpoint allow-list

`https://` only, no userinfo, no port, host equal to or ending in `.` + one of:
`fcm.googleapis.com`, `push.services.mozilla.com`, `push.apple.com`, `notify.windows.com`.
Max length 1023 bytes, printable ASCII without spaces.

### Reminders

- End reminder due: booking `active`, not `end_reminded`, `end_reminder_before_min > 0`,
  `booked_end - before <= now < booked_end`, and the trip started before that window
  (`actual_start < booked_end - before`; a short trip started inside the window is not reminded).
  Cleared when `booked_end` or `user_id` changes (as `late_reminded`).
- Channel per due reminder: user has subscriptions -> push to each; delivered = at least one 2xx.
  404/410 -> subscription deleted. Not delivered (no subscription, all failed) -> mail, with today's
  retry rule (a failed mail ends the tick; retried on the next 5-min tick).
- Push request: `POST endpoint`, headers `TTL: 3600`, `Urgency: high`,
  `Content-Encoding: aes128gcm`, `Content-Type: application/octet-stream`,
  `Authorization: vapid t=<JWT>, k=<public key>`. JWT: `aud` = endpoint origin, `exp` = now + 12 h,
  `sub` = `base_url` if https, else `mailto:<mail_from>` if set, else omitted. Timeout 5 s.
- Payload JSON `{"title","body","url":"/#/buchungen","tag":"booking-<id>"}`:
  - start: `Fahrt ab HH:MM` / `<car>: Starten kannst du ab HH:MM.`
  - end: `Rückgabe bis HH:MM` / `<car>: Bitte rechtzeitig zurückbringen oder die Buchung verlängern.`
  - late: `Rückgabe überfällig: <car>` / `Die Buchung ist um HH:MM abgelaufen. Bitte bring das Auto zurück und beende die Fahrt.`
- End reminder mail: subject `Erinnerung: Rückgabe bis HH:MM`; body like the late reminder, plus the
  late-fee line when the tariff has a late fee and it is no club trip.

### API

- `GET /api/push/key` (user): `{"key":"<base64url public key>"}`.
- `POST /api/me/push` (user): `{endpoint, p256dh, auth}` -> `{"ok":true}`; 400 `invalid`, 400
  `push_endpoint` for hosts outside the allow-list.
- `DELETE /api/me/push` (user): `{endpoint}` -> `{"ok":true}`; removes only the caller's own row.
- `POST /api/me/push/test` (user): sends a test push to all of the caller's subscriptions:
  `{"sent":n,"failed":m}`.
- Admin settings GET/PUT gain `end_reminder_before_min`.

### Client

- `web/js/push.js`: support detection, state (`unsupported`, `ios-install`, `denied`, `off`, `on`),
  enable (permission, subscribe with server key, POST), disable (DELETE, unsubscribe), sync at app
  start when logged in and permission is granted (re-POST; re-subscribe if the server key changed),
  and drop on logout (DELETE, unsubscribe).
- „Konto“: card „Benachrichtigungen“ with state text, enable/disable and test buttons.
- Service worker: `push` shows the notification; `notificationclick` focuses an open app window and
  navigates it to `url`, else opens one.
- Admin settings: field „Erinnerung vor Buchungsende (Minuten, 0 = aus)“.

### Docs

spec §5 (data), §10 (notifications), API list, privacy (push services see only timing and the
device endpoint), backup (vapid.json); help page (members: enable push, iPhone install first);
deploy README (`libssl-dev`, outbound HTTPS to push services).

## Left to the builder

File layout, function names, test structure; exact German UI copy beyond what is fixed above.
