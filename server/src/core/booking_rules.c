#include "core/booking_rules.h"

/*
 * Check order for create/update (first failure wins):
 *   BR_INVALID -> BR_FORBIDDEN -> BR_NOT_MODIFIABLE -> BR_CLUB_FORBIDDEN -> BR_NOT_EXTENSION
 *   -> BR_CAR_INACTIVE -> BR_OVERLAP -> BR_IN_PAST
 * Check order for start (first failure wins):
 *   BR_FORBIDDEN -> BR_NOT_MODIFIABLE -> BR_EXPIRED -> BR_CAR_INACTIVE -> BR_CAR_BUSY
 * BR_INVALID covers times (range per br_time_in_range, order, 60 s alignment) and existence of the car and,
 * for admin writes, of an active target user.
 * An admin update resulting in BK_DONE (closing a trip, entering km) skips both "active" checks: car and
 * user only have to exist, so finished trips stay editable after a deactivation.
 */

static const car_t *find_car(const db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_cars; i++)
    if (db->cars[i].id == id) return &db->cars[i];
  return NULL;
}

static const user_t *find_user(const db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_users; i++)
    if (db->users[i].id == id) return &db->users[i];
  return NULL;
}

bool br_time_in_range(int64_t t, int64_t now) {
  if (t < 0) return false;
  if (now > INT64_MAX - BR_TIME_MAX_AHEAD) return true;
  return t <= now + BR_TIME_MAX_AHEAD;
}

bool interval_overlaps(int64_t a0, int64_t a1, int64_t b0, int64_t b1) {
  return a0 < b1 && b0 < a1;
}

int64_t booking_occupied_start(const booking_t *b) {
  if (b->state == BK_ACTIVE && b->actual_start > 0 && b->actual_start < b->booked_start)
    return b->actual_start;
  return b->booked_start;
}

int64_t booking_occupied_end(const booking_t *b, int64_t now) {
  if (b->state == BK_ACTIVE && now >= b->booked_end) return now + BR_OVERDUE_GRACE;
  return b->booked_end;
}

br_result_t br_check_free(const db_t *db, int64_t car_id, int64_t start, int64_t end,
                          int64_t exclude_id, int64_t now) {
  for (size_t i = 0; i < db->n_bookings; i++) {
    const booking_t *b = &db->bookings[i];
    if (b->car_id != car_id || b->id == exclude_id) continue;
    if (b->state != BK_RESERVED && b->state != BK_ACTIVE) continue;
    if (interval_overlaps(start, end, booking_occupied_start(b), booking_occupied_end(b, now)))
      return BR_OVERLAP;
  }
  return BR_OK;
}

static bool times_valid(const booking_t *b, int64_t now) {
  return br_time_in_range(b->booked_start, now) && br_time_in_range(b->booked_end, now) &&
         b->booked_start < b->booked_end && b->booked_start % 60 == 0 && b->booked_end % 60 == 0;
}

static bool start_allowed(const db_t *db, int64_t start, int64_t now) {
  return start >= now - (int64_t)db->settings.start_tolerance_min * 60;
}

/* Shared INVALID checks for a booking being written. */
static bool write_valid(const db_t *db, const user_t *actor, const booking_t *nb, int64_t now) {
  if (!times_valid(nb, now) || !find_car(db, nb->car_id)) return false;
  if (actor->role == ROLE_ADMIN) {
    const user_t *u = find_user(db, nb->user_id);
    if (!u || (!u->active && nb->state != BK_DONE)) return false;
  }
  return true;
}

br_result_t br_check_create(const db_t *db, const user_t *actor, const booking_t *nb, int64_t now) {
  if (!write_valid(db, actor, nb, now)) return BR_INVALID;
  bool admin = actor->role == ROLE_ADMIN;
  if (!admin && nb->user_id != actor->id) return BR_FORBIDDEN;
  if (!admin && nb->club_trip && !actor->may_club_trip) return BR_CLUB_FORBIDDEN;
  if (!find_car(db, nb->car_id)->active) return BR_CAR_INACTIVE;
  br_result_t r = br_check_free(db, nb->car_id, nb->booked_start, nb->booked_end, 0, now);
  if (r != BR_OK) return r;
  if (!admin && !start_allowed(db, nb->booked_start, now)) return BR_IN_PAST;
  return BR_OK;
}

br_result_t br_check_update(const db_t *db, const user_t *actor, const booking_t *old,
                            const booking_t *nb, int64_t now) {
  if (!write_valid(db, actor, nb, now)) return BR_INVALID;
  bool admin = actor->role == ROLE_ADMIN;
  if (!admin && old->user_id != actor->id) return BR_FORBIDDEN;
  if (old->state == BK_CANCELLED || (!admin && old->state == BK_DONE)) return BR_NOT_MODIFIABLE;

  bool start_changed = nb->booked_start != old->booked_start;
  bool flag_changed = nb->club_trip != old->club_trip;
  if (!admin && flag_changed) {
    /* The flag is fixed once the booking has started; setting it needs the permission. */
    if (now >= old->booked_start || old->state != BK_RESERVED) return BR_NOT_MODIFIABLE;
    if (nb->club_trip && !actor->may_club_trip) return BR_CLUB_FORBIDDEN;
  }
  if (!admin) {
    bool same_ids = nb->car_id == old->car_id && nb->user_id == old->user_id;
    if (old->state == BK_RESERVED) {
      bool grows = nb->booked_start <= old->booked_start && nb->booked_end >= old->booked_end;
      bool changed = start_changed || nb->booked_end != old->booked_end || flag_changed;
      if (!same_ids || !grows || !changed) return BR_NOT_EXTENSION;
    } else { /* BK_ACTIVE */
      if (!same_ids || start_changed || nb->booked_end < old->booked_end) return BR_NOT_EXTENSION;
    }
  }

  if (nb->state != BK_DONE && !find_car(db, nb->car_id)->active) return BR_CAR_INACTIVE;
  if (nb->state == BK_RESERVED || nb->state == BK_ACTIVE) {
    /* Check what nb will occupy (early actual_start, overdue grace), not just its booked interval. */
    br_result_t r = br_check_free(db, nb->car_id, booking_occupied_start(nb),
                                  booking_occupied_end(nb, now), old->id, now);
    if (r != BR_OK) return r;
  }
  if (!admin && start_changed && !start_allowed(db, nb->booked_start, now)) return BR_IN_PAST;
  return BR_OK;
}

br_result_t br_check_start(const db_t *db, const user_t *actor, const booking_t *b, int64_t now) {
  if (b->user_id != actor->id) return BR_FORBIDDEN;
  if (b->state != BK_RESERVED) return BR_NOT_MODIFIABLE;
  if (now >= b->booked_end) return BR_EXPIRED;
  const car_t *car = find_car(db, b->car_id);
  if (!car || !car->active) return BR_CAR_INACTIVE;
  int64_t from = now < b->booked_start ? now : b->booked_start;
  if (br_check_free(db, b->car_id, from, b->booked_end, b->id, now) != BR_OK) return BR_CAR_BUSY;
  return BR_OK;
}

br_result_t br_check_return(const user_t *actor, const booking_t *b) {
  if (b->user_id != actor->id) return BR_FORBIDDEN;
  if (b->state != BK_ACTIVE) return BR_NOT_MODIFIABLE;
  return BR_OK;
}

br_result_t br_apply_trip(const booking_t *old, booking_t *nb, int64_t now) {
  int64_t s = nb->actual_start, e = nb->actual_end;
  if (s < 0 || s > now || e < 0 || e > now) return BR_INVALID;
  if (s == 0 && e != 0) return BR_INVALID;
  if (s > 0 && e != 0 && e <= s) return BR_INVALID;
  if (s == 0) { nb->state = BK_RESERVED; nb->km_pending = false; }
  else if (e == 0) { nb->state = BK_ACTIVE; nb->km_pending = false; }
  else {
    nb->state = BK_DONE;
    /* Telematics km belong to the old times; manual km stay. */
    if (nb->km_source == KM_TELEMATICS && (s != old->actual_start || e != old->actual_end)) {
      nb->km_source = KM_NONE;
      nb->km_start = nb->km_end = 0;
    }
    nb->km_pending = nb->km_source == KM_NONE;
  }
  return BR_OK;
}

br_result_t br_check_damage(const user_t *actor, const booking_t *b, damage_phase_t phase) {
  if (b->user_id != actor->id) return BR_FORBIDDEN;
  bool ok = phase == DMG_START
      ? b->actual_start > 0 && (b->state == BK_ACTIVE || b->state == BK_DONE)
      : b->state == BK_DONE && b->actual_end > 0;
  return ok ? BR_OK : BR_NOT_MODIFIABLE;
}

br_result_t br_check_cancel(const user_t *actor, const booking_t *b, int64_t now) {
  bool admin = actor->role == ROLE_ADMIN;
  if (!admin && b->user_id != actor->id) return BR_FORBIDDEN;
  if (b->state != BK_RESERVED) return BR_NOT_MODIFIABLE;
  if (!admin && now >= b->booked_start) return BR_IN_PAST;
  return BR_OK;
}

int64_t br_cancel_fee(const settings_t *s, const booking_t *b, int64_t now) {
  if (b->club_trip) return 0;
  if (b->booked_start - now < (int64_t)s->cancel_deadline_min * 60) return s->cancel_fee_cents;
  return 0;
}
