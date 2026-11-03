/* Glue between booking/billing events and the pure mail texts of core/notify (spec §10).
 * Mail failures never change the outcome of the request that triggered them. */
#include <string.h>

#include "core/notify.h"
#include "http/api_internal.h"

int api_notify_booking(app_t *a, notify_kind_t kind, const booking_t *b, int64_t actor_id) {
  const user_t *u = db_user(&a->db, b->user_id);
  const car_t *c = db_car(&a->db, b->car_id);
  if (!u || !u->active || !c) return 1;
  notify_booking_t n = {.kind = kind, .b = b, .car = c, .user_name = u->name, .base_url = a->cfg.base_url,
                        .by_admin = actor_id != b->user_id,
                        .start_tolerance_min = a->db.settings.start_tolerance_min};
  char subject[256], body[4096];
  if (notify_booking_mail(&n, subject, sizeof subject, body, sizeof body) != 0) return -1;
  return api_send_mail(a, u->email, subject, body) == 0 ? 0 : -1;
}

void api_booking_changed(const booking_t *old, booking_t *nb) {
  if (nb->booked_start != old->booked_start) nb->start_reminded = false;
  if (nb->booked_end != old->booked_end) nb->late_reminded = false;
}

bool api_booking_mail_worthy(const booking_t *old, const booking_t *nb) {
  return nb->booked_start != old->booked_start || nb->booked_end != old->booked_end ||
         nb->car_id != old->car_id || nb->club_trip != old->club_trip;
}

/* Due reminder kind for b at now, or -1 (spec §10, rules in plan builder calls 4–6). */
static int reminder_due(const settings_t *s, const booking_t *b, int64_t now) {
  if (b->state == BK_RESERVED && !b->start_reminded && s->reminder_before_min > 0) {
    int64_t from = b->booked_start - (int64_t)s->reminder_before_min * 60;
    if (from <= now && now < b->booked_start && b->created_at < from) return NT_REMIND_START;
  }
  if (b->state == BK_ACTIVE && !b->late_reminded && now >= b->booked_end) return NT_REMIND_LATE;
  return -1;
}

int api_send_reminders(app_t *a, int64_t now) {
  int sent = 0;
  /* db_put_booking may invalidate pointers into the table: index, copy, put. */
  for (size_t i = 0; i < a->db.n_bookings; i++) {
    int kind = reminder_due(&a->db.settings, &a->db.bookings[i], now);
    if (kind < 0) continue;
    booking_t b = a->db.bookings[i];
    const user_t *u = db_user(&a->db, b.user_id);
    const car_t *c = db_car(&a->db, b.car_id);
    if (!u || !u->active || !c) continue;
    notify_booking_t n = {.kind = (notify_kind_t)kind, .b = &b, .car = c, .user_name = u->name,
                          .base_url = a->cfg.base_url,
                          .start_tolerance_min = a->db.settings.start_tolerance_min};
    if (kind == NT_REMIND_LATE) {
      const tariff_t *t = db_tariff_at(&a->db, b.car_id, b.booked_start);
      n.late_fee_cents_per_h = t ? t->late_fee_cents_per_started_h : 0;
    }
    char subject[256], body[4096];
    if (notify_booking_mail(&n, subject, sizeof subject, body, sizeof body) != 0) continue;
    if (api_send_mail(a, u->email, subject, body) != 0) continue;   /* retried on the next call */
    if (kind == NT_REMIND_START) b.start_reminded = true;
    else b.late_reminded = true;
    if (db_put_booking(&a->db, &b) != 0) return -1;
    sent++;
  }
  return sent;
}
