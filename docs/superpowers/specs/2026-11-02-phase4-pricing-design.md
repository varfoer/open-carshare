# Phase 4: Tariffs and pricing — design

> Note: tariffs moved from cars to users in Phase 8; see `2026-11-09-user-tariffs-design.md`.

Date: 2026-11-02. Authority: `docs/spec.md` §4 (tariff), §6 (pricing),
§15 Phase 4. This document adds what the spec leaves open; where they
disagree, the spec wins unless this document says it changes the spec.

## Intent

Members of the Verein should know what a trip will cost before they book,
and see what it did cost afterwards. Admins set the price per car and can
change it without touching past trips. Phase 4 computes and shows prices;
it does not charge anyone (invoices and SEPA are Phases 5–6).

Success: every `done` or `cancelled` booking shows a price that matches a
hand calculation, and the booking form shows an estimate that updates
while the member types.

## The current tariff (Leapmotor T03, product owner, 2026-11-02)

| Field | Value |
|---|---|
| Day rate (06:00–20:00) | 3,00 €/h (`rate_day_cents_per_h = 300`) |
| Night rate (20:00–06:00) | 1,50 €/h (`rate_night_cents_per_h = 150`, `night_start = 1200`, `night_end = 360`) |
| km rate | 0,30 €/km (`rate_km_cents = 30`) |
| Unused booked time | 50 % (`unused_factor_permille = 500`) |
| Time cap | 30 € per calendar day (`time_cap_cents_per_day = 3000`) |
| Combined cap (time + km) | 50 € per calendar day (`combined_cap_cents_per_day = 5000`) |
| Late fee | 15 € per started hour after `B1 + start_tolerance_min` (15 min) (`late_fee_cents_per_started_h = 1500`) |

Worked examples (hand calculations, to become tests):

- Fri 14:00–18:00 booked and used, 40 km: 4 h × 3 € + 40 × 0,30 € = **24,00 €**.
- Same booking, returned 16:00: 2 h × 3 € + 2 h × 1,50 € (unused) + 12 € = **21,00 €**.
- 00:00–24:00 used, 200 km: time 14 × 3 € + 10 × 1,50 € = 57 € → capped 30 €;
  + 60 € km = 90 € → combined cap **50,00 €**.
- Booked until 18:00, returned 18:10: no late fee (within 15 min tolerance),
  the 10 min are active time. Returned 18:20: + 15 €. Returned 19:20: + 30 €.
- Fri 18:00 – Sun 18:00 touches 3 calendar days: combined ceiling 3 × 50 € = 150 €.

## Goals

1. Tariff per car, versioned (`valid_from`), admin create/list; a new
   version applies to bookings whose booked start is at or after its
   `valid_from` (spec §4: "tariff valid at the booking's booked start").
2. Pricing engine (spec §6) as a pure function in `core/`, exhaustively
   unit-tested, including DST days, both caps, late fee, cancel fee and
   club trips.
3. Estimate in the booking form (new booking and extension).
4. Actual price in the member's history and in the admin booking list.

## Non-goals

- Charging, invoices, SEPA, billing run (Phases 5–6).
- Protection against fake tracker readings: stays in the backlog item
  "Tracker abuse with a known IMEI". Product owner, 2026-11-02: the Verein
  trusts its members; not needed before billing either.
- Storing prices. Prices are computed when shown (see "Decided" below);
  freezing them is the billing run's job.

## Anti-goals

- The estimate and the final price must never be computed by two
  different implementations (browser and server). One engine, on the server.
- No float anywhere in money or time arithmetic (Global Constraints).
- A tariff edit must never change the price of a booking it does not
  apply to.

## Booking form estimate (product owner chose mockup C)

- New field "Geplante Strecke" (km, integer ≥ 0), prefilled with 15.
- Below it: "Voraussichtlich <total>" and a "Details" link. Tapping
  "Details" unfolds the breakdown:
  - one line per rate actually used, e.g. `4 h Tag × 3,00 €` / `12,00 €`,
    `2 h Nacht × 1,50 €`;
  - `15 km × 0,30 €` / `4,50 €`;
  - a line `Deckel` with the negative amount when the time cap or the
    combined cap lowers the price;
  - the total;
  - a short note: `Nicht genutzte Zeit 50 %, Verspätung 15,00 € je angefangene Stunde.`
    (values from the tariff).
- The total updates whenever start, end or km change.
- The estimate assumes the whole booked time is used (no unused time, no
  late fee, no cancel fee) and the planned km.
- Extending a booking shows the estimate for the extended booking.
- Planned km are not stored; they exist only for the estimate.
- Club trip checkbox ticked (if the member may book club trips): estimate
  is 0 €.

## Decided

Product owner (2026-11-02):
- the tariff values above;
- estimate before booking, planned km prefilled 15, layout C;
- tracker abuse protection deferred.
- **Cancelled booking costs only its `cancel_fee_cents`** (frozen on the
  booking at cancel time; club trip 0), no time or km cost and no tariff
  needed. This changes spec §6: the formula applies to `done` bookings; a
  `done` booking that never started (expired reservation, no-show) is priced
  by the formula, i.e. its whole booked time as unused time. Update spec §6
  accordingly in the plan.
- **Backdating**: the first tariff version of a car may have any
  `valid_from` (so bookings from Phases 1–3 get a price); every later
  version needs `valid_from >= now`. Combined with append-only versions, a
  price change never rewrites past trips.
- **Late fee tolerance stays a club-wide setting**: changing
  `start_tolerance_min` changes the late fee shown for past bookings until
  the billing run (Phase 6) freezes prices. Accepted; no per-booking copy.

Designer's calls (not discussed with the product owner; the builder may
revisit with a reason):
- **Estimate endpoint:** the server computes the estimate
  (`GET /api/price-estimate?car_id&start&end&km`, member auth), using the
  same pure engine as the final price. Reason: anti-goal "one engine".
- **Prices computed on read**, not stored: `booking_to_json` responses for
  history and admin lists carry a `price` object computed with the tariff
  valid at `booked_start`. Tariff versions are append-only, so the result
  is stable.
- **Tariff versions are append-only**: admin can add a version with a
  `valid_from`; editing or deleting a version whose `valid_from` is in the
  past is not possible (that would rewrite past prices). A version with a
  future `valid_from` may be deleted.
- **Minutes, exact integer arithmetic** (spec §3: granularity minutes,
  rounding per pricing component, half up): actual times are truncated to
  the whole minute before pricing (`t - t % 60`; booked times already are),
  so a return at 18:15:30 with booked end 18:00 counts 15 late minutes and
  stays within the tolerance. Per calendar day the engine sums
  `rate_cents_per_h × minutes × factor_permille` exactly (factor 1000 for
  active, `unused_factor_permille` for unused), then divides by
  `60 × 1000` once, rounding half up: the day's time cost is one
  component, rounded before the day's time cap. km cost = `km ×
  rate_km_cents` (exact). Late fee and cancel fee are whole cents already.
- **Cap value 0 means "no cap"** for `time_cap_cents_per_day` and
  `combined_cap_cents_per_day`.
- **Local time without the C library's time zone database**: `core/`
  gets a pure `Europe/Berlin` converter (CET/CEST, EU rule: summer time
  from last Sunday of March 01:00 UTC to last Sunday of October 01:00 UTC).
  Day boundaries and the night window use it.
- **Price states**: a `done` booking with `km_pending` shows the time part
  and `km ausstehend` instead of a total; a car or date with no tariff
  shows `Kein Tarif` (booking is still allowed; admin is responsible for
  setting a tariff).
- **Admin UI**: tariffs live on the admin car page, per car: list of
  versions (newest first) and a form "Neuer Tarif ab <Datum>" prefilled
  with the current version's values; money entered in euros with comma
  (`3,00`), stored in cents.

## Constraints

- All existing Global Constraints (C11, `int64_t` UTC seconds, no float,
  German UI / English code, fixed `char[]` reject-not-truncate,
  `-Wall -Wextra -Werror`, ASan/UBSan tests, db copy-before-modify, append
  for growing tables, `web/sw.js` SHELL and CACHE bumped per web change).
- Data written by Phases 1–3 must still load (no tariff file = no tariffs).
- Money: `int64_t` cents.

## Left to the builder

- File layout, function names, JSON key names (beyond `price` on bookings
  and the estimate endpoint's existence), storage file name for tariffs.
- Exact breakdown wording beyond the lines above.
- How the form debounces estimate requests.
- Test case selection beyond the worked examples, DST days (last Sunday of
  March and October), both caps, late fee boundaries, cancel fee and club
  trips.
