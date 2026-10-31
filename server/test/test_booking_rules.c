#include <string.h>

#include "core/booking_rules.h"
#include "unity.h"

#define T 1774742400LL
#define H 3600LL

static db_t db;
static car_t cars[2];
static user_t users[3];
static booking_t bks[4];
static int64_t now = T;

#define U1 (&users[0])
#define U2 (&users[1])
#define ADM (&users[2])

void setUp(void) {
  memset(&db, 0, sizeof db);
  memset(cars, 0, sizeof cars);
  memset(users, 0, sizeof users);
  memset(bks, 0, sizeof bks);
  cars[0].id = 1; cars[0].active = true;
  cars[1].id = 2; cars[1].active = false;
  for (int i = 0; i < 3; i++) { users[i].id = i + 1; users[i].active = true; }
  users[2].role = ROLE_ADMIN;
  db.cars = cars; db.n_cars = 2;
  db.users = users; db.n_users = 3;
  db.bookings = bks; db.n_bookings = 0;
  db.settings.start_tolerance_min = 15;
  db.settings.cancel_deadline_min = 60;
  db.settings.cancel_fee_cents = 500;
  now = T;
}
void tearDown(void) {}

static booking_t *add(int64_t id, int64_t car, int64_t user, int64_t s, int64_t e, booking_state_t st) {
  booking_t *b = &bks[db.n_bookings++];
  b->id = id; b->car_id = car; b->user_id = user; b->booked_start = s; b->booked_end = e; b->state = st;
  return b;
}
static booking_t mk(int64_t car, int64_t user, int64_t s, int64_t e) {
  booking_t b; memset(&b, 0, sizeof b);
  b.car_id = car; b.user_id = user; b.booked_start = s; b.booked_end = e; b.state = BK_RESERVED;
  return b;
}

void test_touching_intervals_do_not_overlap(void) {
  add(1, 1, 2, T + 10 * H, T + 12 * H, BK_RESERVED);
  booking_t a = mk(1, 1, T + 12 * H, T + 13 * H);
  booking_t b = mk(1, 1, T + 8 * H, T + 10 * H);
  booking_t c = mk(1, 1, T + 11 * H + 60, T + 13 * H);
  TEST_ASSERT_EQUAL(BR_OK, br_check_create(&db, U1, &a, now));
  TEST_ASSERT_EQUAL(BR_OK, br_check_create(&db, U1, &b, now));
  TEST_ASSERT_EQUAL(BR_OVERLAP, br_check_create(&db, U1, &c, now));
}
void test_create_start_must_be_minute_aligned(void) {
  booking_t a = mk(1, 1, T + 10 * H + 30, T + 11 * H);
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_create(&db, U1, &a, now));
  booking_t e = mk(1, 1, T + 10 * H, T + 11 * H + 1);
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_create(&db, U1, &e, now));
}
void test_create_invalid_range_and_car(void) {
  booking_t a = mk(1, 1, T + 10 * H, T + 10 * H);
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_create(&db, U1, &a, now));
  booking_t b = mk(99, 1, T + 10 * H, T + 11 * H);
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_create(&db, U1, &b, now));
}
void test_create_tolerance_allows_15min_past(void) {
  booking_t a = mk(1, 1, now - 900, now + H);
  booking_t b = mk(1, 1, now - 960, now + H);
  TEST_ASSERT_EQUAL(BR_OK, br_check_create(&db, U1, &a, now));
  TEST_ASSERT_EQUAL(BR_IN_PAST, br_check_create(&db, U1, &b, now));
}
void test_create_inactive_car(void) {
  booking_t a = mk(2, 1, T + 10 * H, T + 11 * H);
  TEST_ASSERT_EQUAL(BR_CAR_INACTIVE, br_check_create(&db, U1, &a, now));
}
void test_user_cannot_book_for_other_user(void) {
  booking_t a = mk(1, 2, T + 10 * H, T + 11 * H);
  TEST_ASSERT_EQUAL(BR_FORBIDDEN, br_check_create(&db, U1, &a, now));
}
void test_admin_create_rules(void) {
  booking_t a = mk(1, 2, now - 5 * H, now - 4 * H);
  TEST_ASSERT_EQUAL(BR_OK, br_check_create(&db, ADM, &a, now));
  booking_t b = mk(1, 77, T + 10 * H, T + 11 * H);
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_create(&db, ADM, &b, now));
  users[1].active = false;
  booking_t c = mk(1, 2, T + 10 * H, T + 11 * H);
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_create(&db, ADM, &c, now));
}
void test_user_extend_end_ok(void) {
  booking_t *o = add(1, 1, 1, T + 10 * H, T + 11 * H, BK_RESERVED);
  booking_t n = *o; n.booked_end = T + 12 * H;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, U1, o, &n, now));
}
void test_user_extend_start_earlier_ok(void) {
  booking_t *o = add(1, 1, 1, T + 10 * H, T + 11 * H, BK_RESERVED);
  booking_t n = *o; n.booked_start = T + 9 * H;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, U1, o, &n, now));
  n.booked_start = now - 960;
  TEST_ASSERT_EQUAL(BR_IN_PAST, br_check_update(&db, U1, o, &n, now));
}
void test_user_shorten_rejected(void) {
  booking_t *o = add(1, 1, 1, T + 10 * H, T + 12 * H, BK_RESERVED);
  booking_t n = *o; n.booked_end = T + 11 * H;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, o, &n, now));
  n = *o; /* no change at all */
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, o, &n, now));
}
void test_user_move_rejected(void) {
  booking_t *o = add(1, 1, 1, T + 10 * H, T + 12 * H, BK_RESERVED);
  booking_t n = *o; n.booked_start += H; n.booked_end += H;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, o, &n, now));
}
void test_user_extend_into_other_booking_overlap(void) {
  booking_t *o = add(1, 1, 1, T + 10 * H, T + 11 * H, BK_RESERVED);
  add(2, 1, 2, T + 12 * H, T + 13 * H, BK_RESERVED);
  booking_t n = *o; n.booked_end = T + 12 * H + 60;
  TEST_ASSERT_EQUAL(BR_OVERLAP, br_check_update(&db, U1, o, &n, now));
  n.booked_end = T + 12 * H;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, U1, o, &n, now));
}
void test_user_update_other_users_booking_forbidden(void) {
  booking_t *o = add(1, 1, 2, T + 10 * H, T + 11 * H, BK_RESERVED);
  booking_t n = *o; n.booked_end += H;
  TEST_ASSERT_EQUAL(BR_FORBIDDEN, br_check_update(&db, U1, o, &n, now));
}
void test_user_update_done_not_modifiable(void) {
  booking_t *o = add(1, 1, 1, T - 5 * H, T - 4 * H, BK_DONE);
  booking_t n = *o; n.booked_end += H;
  TEST_ASSERT_EQUAL(BR_NOT_MODIFIABLE, br_check_update(&db, U1, o, &n, now));
}
void test_active_booking_start_change_rejected(void) {
  booking_t *o = add(1, 1, 1, T, T + 2 * H, BK_ACTIVE);
  booking_t n = *o; n.booked_start = T - 60;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, o, &n, now));
  n = *o; n.booked_end = T + H;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, o, &n, now));
  n = *o; n.booked_end = T + 3 * H;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, U1, o, &n, now));
}
void test_active_late_booking_blocks_until_now(void) {
  add(1, 1, 1, T, T + 2 * H, BK_ACTIVE);
  now = T + 3 * H;
  booking_t a = mk(1, 2, T + 2 * H, T + 4 * H);
  a.user_id = 2;
  TEST_ASSERT_EQUAL(BR_OVERLAP, br_check_create(&db, U2, &a, now));
  TEST_ASSERT_EQUAL(BR_OVERLAP, br_check_create(&db, ADM, &a, now));
  TEST_ASSERT_EQUAL_INT64(now + BR_OVERDUE_GRACE, booking_occupied_end(&bks[0], now));
}
void test_cancelled_does_not_block(void) {
  add(1, 1, 2, T + 10 * H, T + 12 * H, BK_CANCELLED);
  add(2, 1, 2, T + 10 * H, T + 12 * H, BK_DONE);
  booking_t a = mk(1, 1, T + 10 * H, T + 12 * H);
  TEST_ASSERT_EQUAL(BR_OK, br_check_create(&db, U1, &a, now));
}
void test_free_excludes_id(void) {
  add(1, 1, 1, T + 10 * H, T + 12 * H, BK_RESERVED);
  TEST_ASSERT_EQUAL(BR_OVERLAP, br_check_free(&db, 1, T + 10 * H, T + 11 * H, 0, now));
  TEST_ASSERT_EQUAL(BR_OK, br_check_free(&db, 1, T + 10 * H, T + 11 * H, 1, now));
  TEST_ASSERT_EQUAL(BR_OK, br_check_free(&db, 2, T + 10 * H, T + 11 * H, 0, now));
}
void test_user_cancel_after_start_rejected(void) {
  booking_t *o = add(1, 1, 1, T + 10 * H, T + 11 * H, BK_RESERVED);
  TEST_ASSERT_EQUAL(BR_OK, br_check_cancel(U1, o, now));
  TEST_ASSERT_EQUAL(BR_IN_PAST, br_check_cancel(U1, o, T + 10 * H));
  TEST_ASSERT_EQUAL(BR_OK, br_check_cancel(ADM, o, T + 10 * H));
  TEST_ASSERT_EQUAL(BR_FORBIDDEN, br_check_cancel(U2, o, now));
  o->state = BK_ACTIVE;
  TEST_ASSERT_EQUAL(BR_NOT_MODIFIABLE, br_check_cancel(U1, o, now));
  TEST_ASSERT_EQUAL(BR_NOT_MODIFIABLE, br_check_cancel(ADM, o, now));
}
void test_admin_may_move_and_shorten(void) {
  booking_t *o = add(1, 1, 1, T + 10 * H, T + 12 * H, BK_RESERVED);
  booking_t n = *o; n.booked_start += H; n.booked_end = n.booked_start + 60;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, ADM, o, &n, now));
  n.user_id = 2;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, ADM, o, &n, now));
  n.user_id = 55;
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_update(&db, ADM, o, &n, now));
  o->state = BK_CANCELLED;
  n = *o;
  TEST_ASSERT_EQUAL(BR_NOT_MODIFIABLE, br_check_update(&db, ADM, o, &n, now));
}
void test_cancel_fee_inside_deadline(void) {
  booking_t a = mk(1, 1, now + 30 * 60, now + 3 * H);
  booking_t b = mk(1, 1, now + 90 * 60, now + 3 * H);
  booking_t c = mk(1, 1, now + 60 * 60, now + 3 * H);
  TEST_ASSERT_EQUAL_INT64(500, br_cancel_fee(&db.settings, &a, now));
  TEST_ASSERT_EQUAL_INT64(0, br_cancel_fee(&db.settings, &b, now));
  TEST_ASSERT_EQUAL_INT64(0, br_cancel_fee(&db.settings, &c, now));
}

void test_time_range(void) {
  TEST_ASSERT_TRUE(br_time_in_range(0, now));
  TEST_ASSERT_TRUE(br_time_in_range(now + BR_TIME_MAX_AHEAD, now));
  TEST_ASSERT_FALSE(br_time_in_range(now + BR_TIME_MAX_AHEAD + 1, now));
  TEST_ASSERT_FALSE(br_time_in_range(-60, now));
  TEST_ASSERT_FALSE(br_time_in_range(INT64_MIN, now));
  TEST_ASSERT_TRUE(br_time_in_range(INT64_MAX, INT64_MAX)); /* no overflow */
  booking_t n = {.car_id = 1, .user_id = 1, .booked_start = T + H,
                 .booked_end = T + BR_TIME_MAX_AHEAD + 60, .state = BK_RESERVED};
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_create(&db, ADM, &n, now));
  n.booked_start = INT64_MIN + 8; /* multiple of 60 */
  n.booked_end = T + H;
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_create(&db, ADM, &n, now));
}

void test_club_create_needs_permission(void) {
  booking_t a = mk(1, 1, T + 10 * H, T + 11 * H);
  a.club_trip = true;
  TEST_ASSERT_EQUAL(BR_CLUB_FORBIDDEN, br_check_create(&db, U1, &a, now));
  U1->may_club_trip = true;
  TEST_ASSERT_EQUAL(BR_OK, br_check_create(&db, U1, &a, now));
  U1->may_club_trip = false;
  TEST_ASSERT_EQUAL(BR_OK, br_check_create(&db, ADM, &a, now)); /* admin: any user */
  a.user_id = 1;
}
void test_club_update_flag_rules(void) {
  booking_t *o = add(1, 1, 1, T + 10 * H, T + 11 * H, BK_RESERVED);
  booking_t nb = *o;
  nb.club_trip = true;
  /* flag-only change: no only_extend, but permission needed */
  TEST_ASSERT_EQUAL(BR_CLUB_FORBIDDEN, br_check_update(&db, U1, o, &nb, now));
  U1->may_club_trip = true;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, U1, o, &nb, now));
  /* clearing needs no permission */
  U1->may_club_trip = false;
  o->club_trip = true;
  nb.club_trip = false;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, U1, o, &nb, now));
  /* unchanged flag without permission is fine (revoked later) */
  nb.club_trip = true;
  nb.booked_end += H;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, U1, o, &nb, now));
  /* unchanged times and flag: still not an extension */
  nb = *o;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, o, &nb, now));
  /* flag change plus shrinking: extend-only still applies */
  o->club_trip = false;
  U1->may_club_trip = true;
  nb = *o;
  nb.club_trip = true;
  nb.booked_end -= H / 2;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, o, &nb, now));
}
void test_club_update_flag_after_start_not_modifiable(void) {
  booking_t *o = add(1, 1, 1, T - H, T + H, BK_ACTIVE);
  U1->may_club_trip = true;
  booking_t nb = *o;
  nb.club_trip = true;
  TEST_ASSERT_EQUAL(BR_NOT_MODIFIABLE, br_check_update(&db, U1, o, &nb, now));
  o->club_trip = true;
  nb.club_trip = false;
  TEST_ASSERT_EQUAL(BR_NOT_MODIFIABLE, br_check_update(&db, U1, o, &nb, now));
  /* extending an active club trip keeps working */
  nb = *o;
  nb.booked_end += H;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, U1, o, &nb, now));
  /* reserved but start reached (tolerance window): same */
  booking_t *r = add(2, 1, 1, T, T + H, BK_RESERVED);
  nb = *r;
  nb.club_trip = true;
  TEST_ASSERT_EQUAL(BR_NOT_MODIFIABLE, br_check_update(&db, U1, r, &nb, now));
}
void test_club_admin_override(void) {
  booking_t *o = add(1, 1, 1, T - H, T + H, BK_ACTIVE);
  booking_t nb = *o;
  nb.club_trip = true;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, ADM, o, &nb, now)); /* U1 lacks permission, started */
}
void test_club_cancel_fee_zero(void) {
  booking_t a = mk(1, 1, T + 30 * 60, T + 2 * H);
  TEST_ASSERT_EQUAL_INT64(500, br_cancel_fee(&db.settings, &a, now));
  a.club_trip = true;
  TEST_ASSERT_EQUAL_INT64(0, br_cancel_fee(&db.settings, &a, now));
}

void test_overdue_active_blocks_until_now_plus_grace(void) {
  booking_t *b = add(1, 1, 1, T - 2 * H, T - H, BK_ACTIVE);
  TEST_ASSERT_EQUAL_INT64(T + BR_OVERDUE_GRACE, booking_occupied_end(b, T));
  TEST_ASSERT_EQUAL_INT64(T - H, booking_occupied_end(b, T - H - 60));
}
void test_early_start_occupies_from_actual_start(void) {
  booking_t *b = add(1, 1, 1, T + 2 * H, T + 4 * H, BK_ACTIVE); b->actual_start = T;
  TEST_ASSERT_EQUAL_INT(BR_OVERLAP, br_check_free(&db, 1, T + 1800, T + 5400, 0, T));
}
void test_start_in_window_ok(void) {
  booking_t *b = add(1, 1, 1, T + 10 * 60, T + 2 * H, BK_RESERVED);
  TEST_ASSERT_EQUAL_INT(BR_OK, br_check_start(&db, U1, b, T));
}
void test_early_start_free_ok(void) {
  booking_t *b = add(1, 1, 1, T + 5 * H, T + 6 * H, BK_RESERVED);
  TEST_ASSERT_EQUAL_INT(BR_OK, br_check_start(&db, U1, b, T));
}
void test_early_start_blocked_by_reserved(void) {
  add(1, 1, 2, T + H, T + 2 * H, BK_RESERVED);
  booking_t *own = add(2, 1, 1, T + 3 * H, T + 4 * H, BK_RESERVED);
  TEST_ASSERT_EQUAL_INT(BR_CAR_BUSY, br_check_start(&db, U1, own, T));
}
void test_start_blocked_by_overdue_active(void) {
  booking_t *a1 = add(1, 1, 2, T - 2 * H, T - 600, BK_ACTIVE); a1->actual_start = T - 2 * H;
  booking_t *own = add(2, 1, 1, T - 600, T + H, BK_RESERVED);
  TEST_ASSERT_EQUAL_INT(BR_CAR_BUSY, br_check_start(&db, &users[0], own, T));
}
void test_start_after_booked_end_expired(void) {
  booking_t *b = add(1, 1, 1, T - 2 * H, T - H, BK_RESERVED);
  TEST_ASSERT_EQUAL_INT(BR_EXPIRED, br_check_start(&db, U1, b, T));
}
void test_start_twice_not_modifiable(void) {
  booking_t *b = add(1, 1, 1, T - H, T + H, BK_ACTIVE);
  TEST_ASSERT_EQUAL_INT(BR_NOT_MODIFIABLE, br_check_start(&db, U1, b, T));
}
void test_start_other_user_forbidden(void) {
  booking_t *b = add(1, 1, 1, T, T + H, BK_RESERVED);
  TEST_ASSERT_EQUAL_INT(BR_FORBIDDEN, br_check_start(&db, U2, b, T));
}
void test_start_inactive_car(void) {
  booking_t *b = add(1, 2, 1, T, T + H, BK_RESERVED);
  TEST_ASSERT_EQUAL_INT(BR_CAR_INACTIVE, br_check_start(&db, U1, b, T));
}
void test_return_rules(void) {
  booking_t *b = add(1, 1, 1, T - H, T + H, BK_ACTIVE);
  TEST_ASSERT_EQUAL_INT(BR_OK, br_check_return(U1, b));
  TEST_ASSERT_EQUAL_INT(BR_FORBIDDEN, br_check_return(U2, b));
  b->state = BK_RESERVED;
  TEST_ASSERT_EQUAL_INT(BR_NOT_MODIFIABLE, br_check_return(U1, b));
  b->state = BK_DONE;
  TEST_ASSERT_EQUAL_INT(BR_NOT_MODIFIABLE, br_check_return(U1, b));
}
void test_apply_trip_derives_state(void) {
  booking_t b = mk(1, 1, T - 3 * H, T - H);
  b.km_pending = true;
  TEST_ASSERT_EQUAL_INT(BR_OK, br_apply_trip(&b, T));
  TEST_ASSERT_EQUAL_INT(BK_RESERVED, b.state);
  TEST_ASSERT_FALSE(b.km_pending);
  b.actual_start = T - 3 * H;
  TEST_ASSERT_EQUAL_INT(BR_OK, br_apply_trip(&b, T));
  TEST_ASSERT_EQUAL_INT(BK_ACTIVE, b.state);
  TEST_ASSERT_FALSE(b.km_pending);
  b.actual_end = T - H;
  TEST_ASSERT_EQUAL_INT(BR_OK, br_apply_trip(&b, T));
  TEST_ASSERT_EQUAL_INT(BK_DONE, b.state);
  TEST_ASSERT_TRUE(b.km_pending);
  b.km_source = KM_MANUAL;
  TEST_ASSERT_EQUAL_INT(BR_OK, br_apply_trip(&b, T));
  TEST_ASSERT_FALSE(b.km_pending);
}
void test_apply_trip_invalid(void) {
  booking_t b = mk(1, 1, T - 3 * H, T - H);
  b.actual_end = T - H;
  TEST_ASSERT_EQUAL_INT(BR_INVALID, br_apply_trip(&b, T));
  b.actual_start = T - H;
  TEST_ASSERT_EQUAL_INT(BR_INVALID, br_apply_trip(&b, T));
  b.actual_start = T - 2 * H; b.actual_end = T - 2 * H - 1;
  TEST_ASSERT_EQUAL_INT(BR_INVALID, br_apply_trip(&b, T));
  b.actual_start = T + 1; b.actual_end = 0;
  TEST_ASSERT_EQUAL_INT(BR_INVALID, br_apply_trip(&b, T));
  b.actual_start = T - H; b.actual_end = T + 1;
  TEST_ASSERT_EQUAL_INT(BR_INVALID, br_apply_trip(&b, T));
}
void test_damage_phase_rules(void) {
  booking_t *b = add(1, 1, 1, T - H, T + H, BK_RESERVED);
  TEST_ASSERT_EQUAL_INT(BR_NOT_MODIFIABLE, br_check_damage(U1, b, DMG_START));
  TEST_ASSERT_EQUAL_INT(BR_NOT_MODIFIABLE, br_check_damage(U1, b, DMG_END));
  b->state = BK_ACTIVE; b->actual_start = T - H;
  TEST_ASSERT_EQUAL_INT(BR_OK, br_check_damage(U1, b, DMG_START));
  TEST_ASSERT_EQUAL_INT(BR_NOT_MODIFIABLE, br_check_damage(U1, b, DMG_END));
  TEST_ASSERT_EQUAL_INT(BR_FORBIDDEN, br_check_damage(U2, b, DMG_START));
  b->state = BK_DONE; b->actual_end = T;
  TEST_ASSERT_EQUAL_INT(BR_OK, br_check_damage(U1, b, DMG_START));
  TEST_ASSERT_EQUAL_INT(BR_OK, br_check_damage(U1, b, DMG_END));
}
void test_club_flag_fixed_after_early_start(void) {
  booking_t *o = add(1, 1, 1, T + H, T + 3 * H, BK_ACTIVE); o->actual_start = T - H;
  U1->may_club_trip = true;
  booking_t nb = *o; nb.club_trip = true;
  TEST_ASSERT_EQUAL(BR_NOT_MODIFIABLE, br_check_update(&db, U1, o, &nb, now));
}
void test_admin_reopen_done_checks_overlap(void) {
  booking_t *o = add(1, 1, 1, T - 3 * H, T - H, BK_DONE);
  add(2, 1, 2, T - 2 * H, T + H, BK_RESERVED);
  booking_t nb = *o; nb.state = BK_ACTIVE;
  TEST_ASSERT_EQUAL(BR_OVERLAP, br_check_update(&db, ADM, o, &nb, now));
}
void test_active_extension_overlap(void) {
  booking_t *o = add(1, 1, 1, T - H, T + H, BK_ACTIVE);
  add(2, 1, 2, T + 2 * H, T + 3 * H, BK_RESERVED);
  booking_t nb = *o; nb.booked_end = T + 2 * H + 60;
  TEST_ASSERT_EQUAL(BR_OVERLAP, br_check_update(&db, U1, o, &nb, now));
}
void test_user_change_car_or_user_rejected(void) {
  booking_t *r = add(1, 1, 1, T + 10 * H, T + 11 * H, BK_RESERVED);
  booking_t nb = *r; nb.booked_end += H; nb.user_id = 2;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, r, &nb, now));
  nb = *r; nb.booked_end += H; nb.car_id = 2;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, r, &nb, now));
  booking_t *a = add(2, 1, 1, T - H, T + H, BK_ACTIVE);
  nb = *a; nb.booked_end += H; nb.user_id = 2;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, a, &nb, now));
  nb = *a; nb.booked_end += H; nb.car_id = 2;
  TEST_ASSERT_EQUAL(BR_NOT_EXTENSION, br_check_update(&db, U1, a, &nb, now));
}
void test_unaligned_update_invalid(void) {
  booking_t *o = add(1, 1, 1, T + 10 * H, T + 11 * H, BK_RESERVED);
  booking_t nb = *o; nb.booked_end += H + 1;
  TEST_ASSERT_EQUAL(BR_INVALID, br_check_update(&db, U1, o, &nb, now));
}
void test_admin_update_done_ok(void) {
  booking_t *o = add(1, 1, 1, T - 3 * H, T - H, BK_DONE);
  booking_t nb = *o; nb.booked_start -= H; nb.booked_end += 600;
  TEST_ASSERT_EQUAL(BR_OK, br_check_update(&db, ADM, o, &nb, now));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_touching_intervals_do_not_overlap);
  RUN_TEST(test_create_start_must_be_minute_aligned);
  RUN_TEST(test_create_invalid_range_and_car);
  RUN_TEST(test_create_tolerance_allows_15min_past);
  RUN_TEST(test_create_inactive_car);
  RUN_TEST(test_user_cannot_book_for_other_user);
  RUN_TEST(test_admin_create_rules);
  RUN_TEST(test_user_extend_end_ok);
  RUN_TEST(test_user_extend_start_earlier_ok);
  RUN_TEST(test_user_shorten_rejected);
  RUN_TEST(test_user_move_rejected);
  RUN_TEST(test_user_extend_into_other_booking_overlap);
  RUN_TEST(test_user_update_other_users_booking_forbidden);
  RUN_TEST(test_user_update_done_not_modifiable);
  RUN_TEST(test_active_booking_start_change_rejected);
  RUN_TEST(test_active_late_booking_blocks_until_now);
  RUN_TEST(test_cancelled_does_not_block);
  RUN_TEST(test_free_excludes_id);
  RUN_TEST(test_user_cancel_after_start_rejected);
  RUN_TEST(test_admin_may_move_and_shorten);
  RUN_TEST(test_cancel_fee_inside_deadline);
  RUN_TEST(test_club_create_needs_permission);
  RUN_TEST(test_club_update_flag_rules);
  RUN_TEST(test_club_update_flag_after_start_not_modifiable);
  RUN_TEST(test_club_admin_override);
  RUN_TEST(test_club_cancel_fee_zero);
  RUN_TEST(test_time_range);
  RUN_TEST(test_overdue_active_blocks_until_now_plus_grace);
  RUN_TEST(test_early_start_occupies_from_actual_start);
  RUN_TEST(test_start_in_window_ok);
  RUN_TEST(test_early_start_free_ok);
  RUN_TEST(test_early_start_blocked_by_reserved);
  RUN_TEST(test_start_blocked_by_overdue_active);
  RUN_TEST(test_start_after_booked_end_expired);
  RUN_TEST(test_start_twice_not_modifiable);
  RUN_TEST(test_start_other_user_forbidden);
  RUN_TEST(test_start_inactive_car);
  RUN_TEST(test_return_rules);
  RUN_TEST(test_apply_trip_derives_state);
  RUN_TEST(test_apply_trip_invalid);
  RUN_TEST(test_damage_phase_rules);
  RUN_TEST(test_club_flag_fixed_after_early_start);
  RUN_TEST(test_admin_reopen_done_checks_overlap);
  RUN_TEST(test_active_extension_overlap);
  RUN_TEST(test_user_change_car_or_user_rejected);
  RUN_TEST(test_unaligned_update_invalid);
  RUN_TEST(test_admin_update_done_ok);
  return UNITY_END();
}
