/* Glue between booking/billing events and the pure mail texts of core/notify (spec §10).
 * Mail failures never change the outcome of the request that triggered them. */
#include <stdio.h>
#include <stdlib.h>
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
  bool reassigned = nb->user_id != old->user_id;   /* the new user was never reminded */
  if (reassigned || nb->booked_start != old->booked_start) nb->start_reminded = false;
  if (reassigned || nb->booked_end != old->booked_end) nb->late_reminded = nb->end_reminded = false;
}

void api_notify_booking_update(app_t *a, const booking_t *old, const booking_t *nb, int64_t actor_id) {
  if (nb->user_id != old->user_id) {
    /* Admin moved the booking to another member: the old one loses it, the new one gets it. */
    api_notify_booking(a, NT_CANCELLED, old, actor_id);
    api_notify_booking(a, NT_CREATED, nb, actor_id);
  } else if (nb->booked_start != old->booked_start || nb->booked_end != old->booked_end ||
             nb->car_id != old->car_id || nb->club_trip != old->club_trip) {
    api_notify_booking(a, NT_MODIFIED, nb, actor_id);
  }
}

/* Due reminder kind for b at now, or -1 (spec §10, rules in plan builder calls 4–6). */
static int reminder_due(const settings_t *s, const booking_t *b, int64_t now) {
  if (b->state == BK_RESERVED && !b->start_reminded && s->reminder_before_min > 0) {
    int64_t from = b->booked_start - (int64_t)s->reminder_before_min * 60;
    if (from <= now && now < b->booked_start && b->created_at < from) return NT_REMIND_START;
  }
  if (b->state == BK_ACTIVE && !b->end_reminded && s->end_reminder_before_min > 0) {
    int64_t from = b->booked_end - (int64_t)s->end_reminder_before_min * 60;
    /* A trip started inside the window needs no reminder: the member just saw the end time. */
    if (from <= now && now < b->booked_end && b->actual_start < from) return NT_REMIND_END;
  }
  if (b->state == BK_ACTIVE && !b->late_reminded && now >= b->booked_end) return NT_REMIND_LATE;
  return -1;
}

/* Push to every device of the user; true if at least one push service took the message. */
static bool remind_by_push(app_t *a, const notify_booking_t *n, int64_t user_id, int64_t now) {
  char title[128], text[512], tag[32];
  if (!api_push_has(a, user_id) || notify_booking_push(n, title, sizeof title, text, sizeof text) != 0)
    return false;
  snprintf(tag, sizeof tag, "booking-%lld", (long long)n->b->id);
  char *payload = api_push_payload(title, text, "/#/buchungen", tag);
  int sent = payload ? api_push_user(a, user_id, payload, now, NULL) : 0;
  free(payload);
  return sent > 0;
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
    if (kind == NT_REMIND_LATE || kind == NT_REMIND_END) {
      const tariff_version_t *t = db_tariff_at(&a->db, b.user_id, b.booked_start);
      n.late_fee_cents_per_h = t ? t->late_fee_cents_per_started_h : 0;
    }
    char subject[256], body[4096], to[sizeof u->email];
    if (notify_booking_mail(&n, subject, sizeof subject, body, sizeof body) != 0) continue;
    snprintf(to, sizeof to, "%s", u->email);
    /* Push replaces the mail; without a delivered push the mail goes out. A failed mail is
     * retried on the next call. Stop the tick: with the relay down every further send would
     * block the event loop for another SMTP timeout. */
    if (!remind_by_push(a, &n, b.user_id, now) && api_send_mail(a, to, subject, body) != 0) break;
    if (kind == NT_REMIND_START) b.start_reminded = true;
    else if (kind == NT_REMIND_END) b.end_reminded = true;
    else b.late_reminded = true;
    if (db_put_booking(&a->db, &b) != 0) return -1;
    sent++;
  }
  return sent;
}

/* Mails every active admin. 1 if at least one sent, 0 no admin, -1 all failed. */
static int mail_admins(app_t *a, const char *subject, const char *body) {
  int admins = 0, ok = 0;
  for (size_t i = 0; i < a->db.n_users; i++) {
    const user_t *u = &a->db.users[i];
    if (u->role != ROLE_ADMIN || !u->active) continue;
    admins++;
    char to[sizeof u->email];
    snprintf(to, sizeof to, "%s", u->email);
    if (api_send_mail(a, to, subject, body) == 0) ok++;
  }
  if (!admins) return 0;
  return ok ? 1 : -1;
}

int api_notify_run_sent(app_t *a, const billing_run_t *run, const invoice_t *invs, size_t n,
                        size_t sent_now, const int64_t *failed, size_t n_failed) {
  notify_run_t r = {.run_id = run->id, .period = run->period, .n_invoices = n, .n_sent_now = sent_now,
                    .failed = failed, .n_failed = n_failed, .collection_date = run->collection_date,
                    .base_url = a->cfg.base_url};
  for (size_t i = 0; i < n; i++) {
    r.n_mailed_total += invs[i].mailed_at != 0;
    r.total_cents += invs[i].total_cents;
  }
  char subject[256], body[8192];
  if (notify_run_mail(&r, subject, sizeof subject, body, sizeof body) != 0) return -1;
  return mail_admins(a, subject, body);
}

int api_notify_returned(app_t *a, const booking_t *b) {
  const car_t *c = db_car(&a->db, b->car_id);
  char t[17], subject[256], body[512];
  notify_local_time(b->actual_end, t);
  int sn = snprintf(subject, sizeof subject, "Fahrt beendet: %s", c ? c->name : "?");
  int bn = snprintf(body, sizeof body, "Zur\xC3\xBC" "ckgegeben: %s\n\n%s/#/admin/buchungen\n", t, a->cfg.base_url);
  if (sn < 0 || (size_t)sn >= sizeof subject || bn < 0 || (size_t)bn >= sizeof body) return -1;
  return mail_admins(a, subject, body);
}
