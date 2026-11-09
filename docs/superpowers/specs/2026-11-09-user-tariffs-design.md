# User tariffs — design

Date: 2026-11-09. Authority: `docs/spec.md` §4 (tariff, user), §6
(pricing), §9 (billing blockers), §10 (late-return reminder), §11 (API).
This document changes the spec where it says so; the plan updates the spec.
The Phase 4 design (`2026-11-02-phase4-pricing-design.md`) stays as history;
where it binds a tariff to a car, this document replaces it.

## Intent

Today a tariff belongs to a car. The Verein wants prices per member instead:
a regular tariff covers about 90 % of the members, and a few members pay
more or less according to their individual agreement with the Verein. The
admin chooses the tariff when creating the member. A member never sees a
tariff name, a tariff list or anyone else's prices; they see their own
estimate and their own prices, as today.

Success: two members booking the same car for the same time and km get the
prices of their own tariffs; a club trip is 0 € for everyone; a tariff
change for one member affects only their bookings that start after the
change; the billing run bills every member at their own tariff.

## Goals

1. Tariffs are named, global price lists (not per car), each versioned
   (append-only `valid_from` versions) with exactly the fields a car tariff
   has today.
2. Each user is linked to a tariff by a dated assignment; the assignment in
   force at a booking's booked start decides the tariff.
3. Admin page for tariffs (create, rename, versions) and a tariff select in
   the admin user form (required on creation; later changes dated).
4. Estimate, booking price, history, admin booking list, late-return
   reminder and billing run all price through the booking user's tariff.
5. Club trips still cost 0 €.

## Non-goals

- Per-car price differences. A tariff applies to every car. (If the Verein
  ever wants that, it is a new design.)
- Showing members their tariff name or any other tariff.
- Migrating deployed data: the app is not deployed (product owner, memory
  2026-11), the storage format simply changes.
- Changing the pricing engine (`core/pricing.c`): the formula, caps, late
  fee, cancel fee and club-trip rule are untouched.

## Anti-goals

- A member endpoint must never return a tariff name or a list of tariffs.
- A tariff or assignment change must never change the price of a booking
  that starts before the change (same append-only rule as today).
- No second pricing implementation; the browser keeps calling the server
  for the estimate.

## Data model (changes spec §4)

### tariff (named price list)
`id`, `name` (1..63 bytes UTF-8, unique after trim, case-sensitive),
`created_at`. No car reference.

### tariff_version (what `tariff` is today, re-parented)
`id`, `tariff_id` (replaces `car_id`), `valid_from`, and unchanged:
`rate_day_cents_per_h`, `rate_night_cents_per_h`, `night_start`,
`night_end`, `rate_km_cents`, `unused_factor_permille`,
`time_cap_cents_per_day`, `combined_cap_cents_per_day`,
`late_fee_cents_per_started_h`, `created_at`. Validation as today
(`tariff_valid`). The version in force at `ts` is the one with the largest
`valid_from <= ts`.

### user_tariff (assignment)
`id`, `user_id`, `tariff_id`, `valid_from`, `created_at`, `created_by`
(admin user id). Append-only. The assignment in force at `ts` is the one
with the largest `valid_from <= ts`; none means "no tariff".

### user
Unchanged fields. The user JSON on member endpoints (`/api/me`, login) carries
nothing about tariffs.

### Tariff lookup (changes spec §6 and §4 "Pricing uses…")
The tariff of a booking is found by `user_id` and `booked_start`: the user's
assignment in force at `booked_start`, then that tariff's version in force
at `booked_start`. Either missing means "no tariff" (`PRICE_NO_TARIFF`,
„Kein Tarif“), exactly like a car without a tariff today. The estimate uses
the calling member and the planned start.

Consequences the builder must keep in mind:
- Billing (§9): the `no_tariff` blocker keeps its meaning (included
  non-cancelled non-club bookings without a tariff at `booked_start`), the
  lookup changes to user-based.
- Late-return reminder (§10): the late fee named in the mail comes from the
  booking user's tariff at `booked_start`.
- Admin-created bookings for a member are priced with that member's tariff.
- Prices stay computed on read; nothing about tariffs is stored on the
  booking. Finalized invoices are frozen as today.

## Rules

### Tariffs (admin)
- Create with a name only. A tariff without a version prices as "no
  tariff" until the admin adds one.
- Rename anytime; names are unique (409 `duplicate_name`,
  „Diesen Tarifnamen gibt es schon.“).
- Versions: same rules as today — the first version of a tariff may have any
  `valid_from` (so bookings of newly created members with backdated
  assignments can be priced); every later version needs `valid_from >=
  now` (409 `tariff_in_past`); no two versions with the same `valid_from`
  (409 `duplicate_valid_from`); only a version with `valid_from > now` can
  be deleted (409 `tariff_active`). Messages as today.
- Delete a tariff only while no assignment (past, current or future)
  references it (409 `tariff_in_use`, „Dieser Tarif ist Nutzern
  zugeordnet.“). Deleting a tariff deletes its versions.

### Assignments (admin)
- User creation (`POST /api/admin/users`) requires `tariff_id` (400
  `invalid` when missing or unknown); the first assignment gets
  `valid_from = now`. This applies to every role, admins book too.
- Later assignments (a tariff switch): `POST /api/admin/users/{id}/tariffs`
  with `tariff_id` and `valid_from`; `valid_from >= now` (409
  `assignment_in_past`, „Ein Tarifwechsel darf erst ab heute gelten.“), no
  two assignments of a user with the same `valid_from` (409
  `duplicate_valid_from`). Assigning the tariff already in force at that
  time is allowed (no-op in effect).
- **Repair path (product owner, 2026-11-09):** the user's *newest*
  assignment may be edited (`PUT /api/admin/user-tariffs/{id}` with
  `tariff_id` and/or `valid_from`, any `valid_from`, backdating included)
  or deleted (`DELETE /api/admin/user-tariffs/{id}`), also when it is in
  force or the user's only one. Refused with 409 `assignment_billed`
  („Es gibt schon abgerechnete Fahrten in diesem Zeitraum.“) when a booking
  of that user with `invoice_id != 0` has `booked_start >=
  min(old valid_from, new valid_from)` (for delete: `>= old valid_from`).
  The new `valid_from` must stay greater than the previous assignment's
  (409 `duplicate_valid_from` on equality, 400 `invalid` when smaller).
  Older assignments are never editable or deletable. Invoices stay frozen;
  every unbilled booking in the affected range re-prices on the next read.
  This is a correction tool; a regular tariff change is the forward-dated
  switch above.
- A user without any assignment is a supported state („Tarif: keiner“):
  the bootstrap admin from `ocs-server create-admin` has none, and a
  deleted only-assignment leaves none. Their bookings price as "no tariff";
  the switch endpoint adds the first assignment (then with `valid_from >=
  now`; backdating that one afterwards goes through the repair path).
- `PUT /api/admin/users/{id}` ignores `tariff_id`; tariff changes go through
  assignments only.
- Deactivating a user leaves assignments untouched.

### Members
- `GET /api/price-estimate?start&end&km[&club]` — the `car` parameter is
  dropped; the estimate is priced with the caller's tariff. (The booking
  form still needs a car for overlap, not for the price.)
- `price.tariff` in estimate and booking JSON stays the version object
  used (its values drive the breakdown note „Nicht genutzte Zeit 50 %, …“).
  It carries no name. The builder may leave the ids in it.
- History, "Meine Buchungen" and the breakdown are otherwise unchanged.

### Club trips
Unchanged: `club_trip` prices everything to 0 regardless of tariff or
missing tariff; the estimate with the club checkbox shows 0 €.

## API (changes spec §11)

Removed: `GET/POST /api/admin/cars/{id}/tariffs`,
`DELETE /api/admin/tariffs/{id}` (old meaning).

Admin, all `MIN_ADMIN`:

```
GET    /api/admin/tariffs                      [{id,name,created_at,versions:[…newest first]}]
POST   /api/admin/tariffs                      {name} → 201 tariff (versions: [])
PUT    /api/admin/tariffs/{id}                 {name} → 200 tariff
DELETE /api/admin/tariffs/{id}                 409 tariff_in_use
POST   /api/admin/tariffs/{id}/versions        {valid_from, rate_…} → 201 version
DELETE /api/admin/tariff-versions/{id}         409 tariff_active
GET    /api/admin/users/{id}/tariffs           [{id,tariff_id,valid_from,created_at,created_by} newest first]
POST   /api/admin/users/{id}/tariffs           {tariff_id, valid_from} → 201 assignment
PUT    /api/admin/user-tariffs/{id}            {tariff_id?, valid_from?} newest only; 409 assignment_billed
DELETE /api/admin/user-tariffs/{id}            newest only; 409 assignment_billed
```

`POST /api/admin/users` gains required `tariff_id`. `GET /api/admin/users`
rows gain `tariff_id` (assignment in force now; 0 = none) so the user list
can show the tariff name without one request per user.

GDPR export (`GET /api/admin/users/{id}/export`) includes the user's
assignments (id, tariff name, valid_from).

## Admin UI

- New route `#/admin/tarife` ("Tarife") in the admin tab row next to
  Nutzer/Autos/Buchungen/Abrechnung/Einstellungen. Per tariff: name (inline
  rename), delete button (disabled or error when in use), version list
  newest first with `ab <Datum>: <tariffSummary>` and delete for future
  versions, and the existing "Neuer Tarif ab <Datum>" form moved here as
  "Neue Version ab <Datum>", prefilled with the tariff's newest version, or
  with the newest version of the first tariff in the list when the tariff
  has none. Form "Neuer Tarif" with a name field at the bottom.
- The admin car page loses its tariff section.
- Admin user form: on creation a required select "Tarif" (tariffs by name;
  the first tariff in the list preselected). On edit: the assignment list
  newest first („ab <Datum>: <name>“; the one in force marked, none →
  „Tarif: keiner“). The newest row has "Bearbeiten" (select + date-time,
  any date, saves via `PUT`) and "Löschen"; older rows are read-only. Below
  it the form "Tarif wechseln": select + date-time (prefilled with the next
  15-minute slot, as the version form does) + button. The user list table
  gets a column "Tarif".
- Member UI: no change except the estimate request without `car`.

## Storage

`data/tariffs.jsonl` (named tariffs), `data/tariff_versions.jsonl`,
`data/user_tariffs.jsonl`; a missing file means none. Same JSONL
copy-before-modify/append conventions as the other tables. Any existing
`tariffs.jsonl` written by the car-bound code is not migrated (app not
deployed); test fixtures are rewritten.

## Constraints

Unchanged Global Constraints: C11, `int64_t` UTC seconds, cents, no float,
German UI / English code, fixed `char[]` reject-not-truncate,
`-Wall -Wextra -Werror`, ASan/UBSan tests, append-only growing tables,
`web/sw.js` SHELL and CACHE bumped per web change. `docs/spec.md` §4, §6,
§9, §10, §11 and §15 are updated in the same change (new phase entry
"User tariffs").

## Tests the plan must cover

- Lookup: assignment and version boundaries (`valid_from == ts` counts;
  no assignment; assignment but tariff without version; future assignment
  not yet in force; a later assignment overrides for bookings starting
  after it but not before).
- Repair path: edit newest assignment backdated re-prices an unbilled
  `done` booking; refused once a booking at/after the range is invoiced;
  older assignment not editable; delete of the only assignment leaves
  „keiner“ and `no_tariff`; `valid_from` not greater than the previous
  assignment's is refused.
- API: all 409 codes above; user creation without/with unknown
  `tariff_id`; estimate uses the caller's tariff and ignores other users';
  two users, same car and times, different totals; club trip 0 € on any
  tariff; member JSON (`/api/me`, bookings, estimate) contains no tariff
  name; delete of an unassigned tariff succeeds and removes its versions;
  delete of an assigned tariff is refused, also when the only assignment
  is a future one (deleting that future assignment frees it).
- Billing: draft lines of two users on different tariffs; `no_tariff`
  blocker for a user whose assignment points at a tariff without a version.
- Late-return reminder names the fee of the user's tariff.
- Existing pricing engine tests stay untouched.

## Left to the builder

- C function and file names, struct names (`tariff_t` may keep its name for
  the version or be renamed), JSON key names beyond those listed, how
  `GET /api/admin/tariffs` embeds versions.
- Whether `price.tariff` keeps `id`/`tariff_id`.
- Exact wording of the new German messages beyond those quoted.
- Layout details of the "Tarife" page and the user form within the
  existing admin style (`admin_ui.js` helpers).
