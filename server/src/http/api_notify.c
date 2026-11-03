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
