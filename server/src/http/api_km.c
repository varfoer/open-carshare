#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/km_resolve.h"
#include "http/api.h"

#define KM_MAIL_AFTER_S 86400
/* A post-trip reading must have been received this long ago before km are
 * resolved: the trip-end backlog may still be on its way (newest-first). */
#define KM_SETTLE_S 600

int api_resolve_km(app_t *a, int64_t now) {
  int n = 0;
  /* db_put_booking may invalidate pointers into the table: index, copy, put. */
  for (size_t i = 0; i < a->db.n_bookings; i++) {
    const booking_t *p = &a->db.bookings[i];
    if (p->state != BK_DONE || !p->km_pending || p->actual_start == 0 || p->actual_end == 0) continue;
    const car_t *c = db_car(&a->db, p->car_id);
    if (!c || !c->tracker_imei[0]) continue;
    int64_t km;
    if (km_resolve(a->db.readings, a->db.n_readings, p->car_id, p->actual_start, p->actual_end,
                   now - KM_SETTLE_S, &km) != KMR_OK)
      continue;
    booking_t b = *p;
    b.km = km;
    b.km_source = KM_TELEMATICS;
    b.km_pending = false;
    b.modified_at = now;
    b.modified_by = 0;
    if (db_put_booking(&a->db, &b) != 0) return -1;
    n++;
  }
  return n;
}

int api_km_pending_mail(app_t *a, int64_t now) {
  int64_t day = now / 86400;
  if (a->db.km_mail_day == day) return 0;
  static const char head[] = "Für folgende Fahrten fehlen seit über 24 Stunden die km vom Tracker:\n\n";
  static const char tail1[] = "\nBitte Tracker prüfen oder km manuell eintragen:\n";
  static const char tail2[] = "/#/admin/buchungen\n";
  size_t cap = sizeof head + sizeof tail1 + sizeof tail2 + sizeof a->cfg.base_url;
  size_t cnt = 0;
  for (size_t i = 0; i < a->db.n_bookings; i++) {
    const booking_t *p = &a->db.bookings[i];
    if (p->state != BK_DONE || !p->km_pending || now - p->actual_end <= KM_MAIL_AFTER_S) continue;
    const car_t *c = db_car(&a->db, p->car_id);
    if (!c || !c->tracker_imei[0]) continue;
    cap += 32 + sizeof c->name + sizeof c->plate + sizeof ((user_t *)0)->name;
    cnt++;
  }
  if (cnt == 0) return 0;
  char *body = malloc(cap);
  if (!body) return -1;
  size_t len = (size_t)snprintf(body, cap, "%s", head);
  for (size_t i = 0; i < a->db.n_bookings; i++) {
    const booking_t *p = &a->db.bookings[i];
    if (p->state != BK_DONE || !p->km_pending || now - p->actual_end <= KM_MAIL_AFTER_S) continue;
    const car_t *c = db_car(&a->db, p->car_id);
    if (!c || !c->tracker_imei[0]) continue;
    const user_t *u = db_user(&a->db, p->user_id);
    len += (size_t)snprintf(body + len, cap - len, "Buchung %lld: %s (%s), %s\n", (long long)p->id,
                            c->name, c->plate, u ? u->name : "?");
  }
  snprintf(body + len, cap - len, "%s%s%s", tail1, a->cfg.base_url, tail2);

  int sent = 0;
  for (size_t i = 0; i < a->db.n_users; i++) {
    const user_t *u = &a->db.users[i];
    if (u->role != ROLE_ADMIN || !u->active) continue;
    char to[sizeof u->email];
    snprintf(to, sizeof to, "%s", u->email);
    if (api_send_mail(a, to, "km ausstehend", body) == 0) sent++;
  }
  free(body);
  if (sent == 0) return -1;
  if (db_put_km_mail_day(&a->db, day) != 0) return -1;
  return 1;
}
