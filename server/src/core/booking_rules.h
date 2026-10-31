#ifndef OCS_CORE_BOOKING_RULES_H
#define OCS_CORE_BOOKING_RULES_H

#include <stdbool.h>
#include <stdint.h>
#include "core/model.h"
#include "store/db.h"

typedef enum { BR_OK = 0, BR_INVALID, BR_IN_PAST, BR_OVERLAP, BR_CAR_INACTIVE,
               BR_NOT_EXTENSION, BR_NOT_MODIFIABLE, BR_FORBIDDEN,
               BR_CLUB_FORBIDDEN, BR_CAR_BUSY, BR_EXPIRED } br_result_t;

/* An active booking at or past its booked end blocks the car until now plus this many seconds. */
#define BR_OVERDUE_GRACE 1800

/* Latest accepted API time, relative to now: two years. */
#define BR_TIME_MAX_AHEAD ((int64_t)2 * 365 * 86400)
/* API-supplied times (booking start/end, license_checked_at) must lie in
 * [0, now + BR_TIME_MAX_AHEAD]; this keeps them far below the codec's 2^53
 * limit and keeps time arithmetic free of overflow. */
bool    br_time_in_range(int64_t t, int64_t now);
bool    interval_overlaps(int64_t a0, int64_t a1, int64_t b0, int64_t b1);   /* half-open */
int64_t booking_occupied_start(const booking_t *b);  /* ACTIVE with actual_start: min(booked_start, actual_start); else booked_start */
int64_t booking_occupied_end(const booking_t *b, int64_t now);  /* ACTIVE and now >= booked_end: now + BR_OVERDUE_GRACE; else booked_end */
br_result_t br_check_free(const db_t *db, int64_t car_id, int64_t start, int64_t end,
                          int64_t exclude_id, int64_t now);   /* only RESERVED/ACTIVE block */
br_result_t br_check_create(const db_t *db, const user_t *actor, const booking_t *nb, int64_t now);
br_result_t br_check_update(const db_t *db, const user_t *actor, const booking_t *old,
                            const booking_t *nb, int64_t now);
br_result_t br_check_start(const db_t *db, const user_t *actor, const booking_t *b, int64_t now);
br_result_t br_check_return(const user_t *actor, const booking_t *b);
/* Admin trip edit: derives nb->state and nb->km_pending from nb->actual_start/actual_end. */
br_result_t br_apply_trip(booking_t *nb, int64_t now);
br_result_t br_check_damage(const user_t *actor, const booking_t *b, damage_phase_t phase);
br_result_t br_check_cancel(const user_t *actor, const booking_t *b, int64_t now);
int64_t br_cancel_fee(const settings_t *s, const booking_t *b, int64_t now);

#endif
