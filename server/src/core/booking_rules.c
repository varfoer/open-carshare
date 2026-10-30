#include "core/booking_rules.h"

/*
 * Check order for create/update (first failure wins):
 *   BR_INVALID -> BR_FORBIDDEN -> BR_NOT_MODIFIABLE -> BR_NOT_EXTENSION
 *   -> BR_CAR_INACTIVE -> BR_IN_PAST -> BR_OVERLAP
 * BR_INVALID covers times (order, 60 s alignment) and existence of the car and,
 * for admin writes, of an active target user.
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

bool interval_overlaps(int64_t a0, int64_t a1, int64_t b0, int64_t b1) {
  return a0 < b1 && b0 < a1;
}

int64_t booking_occupied_end(const booking_t *b, int64_t now) {
  if (b->state == BK_ACTIVE && now > b->booked_end) return now;
  return b->booked_end;
}

br_result_t br_check_free(const db_t *db, int64_t car_id, int64_t start, int64_t end,
                          int64_t exclude_id, int64_t now) {
  for (size_t i = 0; i < db->n_bookings; i++) {
    const booking_t *b = &db->bookings[i];
    if (b->car_id != car_id || b->id == exclude_id) continue;
    if (b->state != BK_RESERVED && b->state != BK_ACTIVE) continue;
    if (interval_overlaps(start, end, b->booked_start, booking_occupied_end(b, now)))
      return BR_OVERLAP;
  }
  return BR_OK;
}

static bool times_valid(const booking_t *b) {
  return b->booked_start < b->booked_end && b->booked_start % 60 == 0 && b->booked_end % 60 == 0;
}

static bool start_allowed(const db_t *db, int64_t start, int64_t now) {
  return start >= now - (int64_t)db->settings.start_tolerance_min * 60;
}

/* Shared INVALID checks for a booking being written. */
static bool write_valid(const db_t *db, const user_t *actor, const booking_t *nb) {
  if (!times_valid(nb) || !find_car(db, nb->car_id)) return false;
  if (actor->role == ROLE_ADMIN) {
    const user_t *u = find_user(db, nb->user_id);
    if (!u || !u->active) return false;
  }
  return true;
}

br_result_t br_check_create(const db_t *db, const user_t *actor, const booking_t *nb, int64_t now) {
  if (!write_valid(db, actor, nb)) return BR_INVALID;
  bool admin = actor->role == ROLE_ADMIN;
  if (!admin && nb->user_id != actor->id) return BR_FORBIDDEN;
  if (!find_car(db, nb->car_id)->active) return BR_CAR_INACTIVE;
  if (!admin && !start_allowed(db, nb->booked_start, now)) return BR_IN_PAST;
  return br_check_free(db, nb->car_id, nb->booked_start, nb->booked_end, 0, now);
}

br_result_t br_check_update(const db_t *db, const user_t *actor, const booking_t *old,
                            const booking_t *nb, int64_t now) {
  if (!write_valid(db, actor, nb)) return BR_INVALID;
  bool admin = actor->role == ROLE_ADMIN;
  if (!admin && old->user_id != actor->id) return BR_FORBIDDEN;
  if (old->state == BK_CANCELLED || (!admin && old->state == BK_DONE)) return BR_NOT_MODIFIABLE;

  bool start_changed = nb->booked_start != old->booked_start;
  if (!admin) {
    bool same_ids = nb->car_id == old->car_id && nb->user_id == old->user_id;
    if (old->state == BK_RESERVED) {
      bool grows = nb->booked_start <= old->booked_start && nb->booked_end >= old->booked_end;
      bool changed = start_changed || nb->booked_end != old->booked_end;
      if (!same_ids || !grows || !changed) return BR_NOT_EXTENSION;
    } else { /* BK_ACTIVE */
      if (!same_ids || start_changed || nb->booked_end < old->booked_end) return BR_NOT_EXTENSION;
    }
  }

  if (!find_car(db, nb->car_id)->active) return BR_CAR_INACTIVE;
  if (!admin && start_changed && !start_allowed(db, nb->booked_start, now)) return BR_IN_PAST;
  if (old->state == BK_RESERVED || old->state == BK_ACTIVE)
    return br_check_free(db, nb->car_id, nb->booked_start, nb->booked_end, old->id, now);
  return BR_OK;
}

br_result_t br_check_cancel(const user_t *actor, const booking_t *b, int64_t now) {
  bool admin = actor->role == ROLE_ADMIN;
  if (!admin && b->user_id != actor->id) return BR_FORBIDDEN;
  if (b->state != BK_RESERVED) return BR_NOT_MODIFIABLE;
  if (!admin && now >= b->booked_start) return BR_IN_PAST;
  return BR_OK;
}

int64_t br_cancel_fee(const settings_t *s, const booking_t *b, int64_t now) {
  if (b->booked_start - now < (int64_t)s->cancel_deadline_min * 60) return s->cancel_fee_cents;
  return 0;
}
