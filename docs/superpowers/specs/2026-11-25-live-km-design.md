# Live km and price during a trip, provisional price after return

Status: approved by the product owner on 2026-11-25 (option A, "Rückgabe jetzt").

## Why

Field test: after a return the km (and so the price) appeared only after a long wait (up to about
75 min: next parked tracker record + 600 s settle + 5-min job). The product owner does not need the
final km sooner; a live view during the trip and a provisional price after the return are enough.
"Only a few km might be missing", shown as „vorläufiger Preis“.

## Goals

- Running trip of a car with a tracker: the booking card shows km so far and the price if the car
  were returned now, refreshed every 60 s while the page is open.
- Returned trip still `km_pending`: „Verlauf“ (and the admin booking list) show the price with the
  km from the readings so far, marked „vorläufiger Preis“.

## Non-goals

- No change to km resolution or billing: billing still waits for the final km (§8).
- No server push of updates; the client polls.
- No positions, no map.

## Approaches

- A „Rückgabe jetzt“ (chosen): price of the booking as if returned now (active time so far, rest of
  the booked time at the unused factor, km so far, late fee if past the end). Equals the invoice when
  returned now.
- B „bis Buchungsende“: full booked time plus km so far. Simpler to read, too high on early return.

## Design

- `km_live(readings, car_id, a0, a1)`: first and last reading with `a0 <= ts <= a1`;
  `km = round((last_m - first_m) / 1000)` half-up (same rounding as `km_resolve`); none when there
  is no reading in the window, the odometer went backwards or it is implausible. Also returns the ts
  of the last reading (`as_of`).
- Booking JSON gains `live` (null otherwise) for cars with `tracker_imei` when the booking is `active`
  (window `actual_start..now`) or `done` with `km_pending` (window `actual_start..actual_end`) and
  `km_live` has a value: `{km, as_of, price}`. `price` is the usual price object, computed by
  `price_booking` on a copy of the booking with `state done`, `actual_end` = window end, `km_start 0`,
  `km_end km` (so club trips, missing tariffs and late fees behave as at billing).
- Web: `trip.js` helpers for the texts. Active card: `Bisher 23 km · 41,50 € bei Rückgabe jetzt`
  plus `Stand HH:MM` (time of the last reading). The card's line refreshes every 60 s from
  `GET /api/bookings` while connected and the page is visible. „Verlauf“ / admin list: done +
  `km_pending` + `live`: `41,50 € (vorläufiger Preis)` and the km line `ca. 23 km (vorläufig)`.
- Tracker docs: moving period 60 s (Home and Roaming tab), so the live km lag at most about a minute.

## Left to the builder

Function names, file layout, test structure.
