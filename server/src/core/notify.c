#include "core/notify.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "core/berlin.h"
#include "core/invoice_doc.h"

void notify_local_time(int64_t utc, char out[17]) {
  time_t t = (time_t)(utc + berlin_offset(utc));
  struct tm tm;
  if (!gmtime_r(&t, &tm) || strftime(out, 17, "%d.%m.%Y %H:%M", &tm) != 16) snprintf(out, 17, "?");
}

/* Appends to buf at *len; on overflow sets *len = cap (sticky failure). */
static void add(char *buf, size_t cap, size_t *len, const char *fmt, ...) {
  if (*len >= cap) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
  va_end(ap);
  *len = n < 0 || (size_t)n >= cap - *len ? cap : *len + (size_t)n;
}

int notify_booking_mail(const notify_booking_t *n, char *subject, size_t scap, char *body, size_t bcap) {
  const booking_t *b = n->b;
  char from[17], to[17], money[32];
  notify_local_time(b->booked_start, from);
  notify_local_time(b->booked_end, to);
  size_t sl = 0, bl = 0;
  if (!scap || !bcap) return -1;
  subject[0] = body[0] = 0;
  switch (n->kind) {
    case NT_CREATED: add(subject, scap, &sl, "Buchung %lld best\xC3\xA4tigt", (long long)b->id); break;
    case NT_MODIFIED: add(subject, scap, &sl, "Buchung %lld ge\xC3\xA4ndert", (long long)b->id); break;
    case NT_CANCELLED: add(subject, scap, &sl, "Buchung %lld storniert", (long long)b->id); break;
    case NT_REMIND_START: add(subject, scap, &sl, "Erinnerung: Fahrt ab %s", from + 11); break;
    case NT_REMIND_LATE: add(subject, scap, &sl, "R\xC3\xBC" "ckgabe \xC3\xBC" "berf\xC3\xA4llig: %s", n->car->name); break;
    case NT_REMIND_END: add(subject, scap, &sl, "Erinnerung: R\xC3\xBC" "ckgabe bis %s", to + 11); break;
    default: return -1;
  }
  add(body, bcap, &bl, "Hallo %s,\n\n", n->user_name);
  switch (n->kind) {
    case NT_CREATED: add(body, bcap, &bl, "deine Buchung ist eingetragen:"); break;
    case NT_MODIFIED:
      add(body, bcap, &bl, "deine Buchung wurde ge\xC3\xA4ndert:%s",
          n->by_admin ? " Die \xC3\x84nderung hat die Vereinsverwaltung vorgenommen." : "");
      break;
    case NT_CANCELLED:
      add(body, bcap, &bl, "deine Buchung wurde storniert:%s",
          n->by_admin ? " Die Stornierung hat die Vereinsverwaltung vorgenommen." : "");
      break;
    case NT_REMIND_START: add(body, bcap, &bl, "deine Fahrt beginnt bald:"); break;
    case NT_REMIND_LATE:
      add(body, bcap, &bl,
          "deine Buchung ist am %s abgelaufen, die Fahrt ist aber noch nicht beendet. Bitte bring das Auto "
          "zur\xC3\xBC" "ck und beende die Fahrt unter \xE2\x80\x9EMeine Buchungen\xE2\x80\x9C.",
          to);
      break;
    case NT_REMIND_END:
      add(body, bcap, &bl,
          "deine Buchung endet bald. Bitte bring das Auto rechtzeitig zur\xC3\xBC" "ck und beende die Fahrt unter "
          "\xE2\x80\x9EMeine Buchungen\xE2\x80\x9C. Brauchst du l\xC3\xA4nger, verl\xC3\xA4ngere die Buchung dort, "
          "solange das Auto frei ist.");
      break;
  }
  add(body, bcap, &bl, "\n\nAuto: %s (%s)\nVon: %s\nBis: %s\n", n->car->name, n->car->plate, from, to);
  if (b->club_trip) add(body, bcap, &bl, "Vereinsfahrt\n");
  if (n->car->location[0] && n->kind != NT_CANCELLED) add(body, bcap, &bl, "Standort: %s\n", n->car->location);
  if (n->kind == NT_CANCELLED && b->cancel_fee_cents > 0) {
    money_de(b->cancel_fee_cents, money, sizeof money);
    add(body, bcap, &bl, "Stornogeb\xC3\xBChr: %s\n", money);
  }
  if (n->kind == NT_REMIND_START) {
    char t[17];
    notify_local_time(b->booked_start - (int64_t)n->start_tolerance_min * 60, t);
    add(body, bcap, &bl, "Starten kannst du ab %s.\n", t);
  }
  if ((n->kind == NT_REMIND_LATE || n->kind == NT_REMIND_END) && n->late_fee_cents_per_h > 0 && !b->club_trip) {
    char t[17];
    notify_local_time(b->booked_end + (int64_t)n->start_tolerance_min * 60, t);
    money_de(n->late_fee_cents_per_h, money, sizeof money);
    add(body, bcap, &bl, "Ab %s kostet jede angefangene Stunde %s Versp\xC3\xA4tungsgeb\xC3\xBChr.\n", t, money);
  }
  add(body, bcap, &bl, "\nMeine Buchungen: %s/#/buchungen\n", n->base_url);
  return sl >= scap || bl >= bcap ? -1 : 0;
}

int notify_booking_push(const notify_booking_t *n, char *title, size_t tcap, char *body, size_t bcap) {
  const booking_t *b = n->b;
  char t[17];
  size_t tl = 0, bl = 0;
  if (!tcap || !bcap) return -1;
  title[0] = body[0] = 0;
  switch (n->kind) {
    case NT_REMIND_START:
      notify_local_time(b->booked_start, t);
      add(title, tcap, &tl, "Fahrt ab %s", t + 11);
      notify_local_time(b->booked_start - (int64_t)n->start_tolerance_min * 60, t);
      add(body, bcap, &bl, "%s: Starten kannst du ab %s.", n->car->name, t + 11);
      break;
    case NT_REMIND_END:
      notify_local_time(b->booked_end, t);
      add(title, tcap, &tl, "R\xC3\xBC" "ckgabe bis %s", t + 11);
      add(body, bcap, &bl, "%s: Bitte rechtzeitig zur\xC3\xBC" "ckbringen oder die Buchung verl\xC3\xA4ngern.",
          n->car->name);
      break;
    case NT_REMIND_LATE:
      notify_local_time(b->booked_end, t);
      add(title, tcap, &tl, "R\xC3\xBC" "ckgabe \xC3\xBC" "berf\xC3\xA4llig: %s", n->car->name);
      add(body, bcap, &bl, "Die Buchung ist um %s abgelaufen. Bitte bring das Auto zur\xC3\xBC" "ck und beende die Fahrt.",
          t + 11);
      break;
    default: return -1;
  }
  return tl >= tcap || bl >= bcap ? -1 : 0;
}

int notify_run_mail(const notify_run_t *r, char *subject, size_t scap, char *body, size_t bcap) {
  char label[40], money[32], date[11];
  if (!scap || !bcap) return -1;
  period_label(r->period, label, sizeof label);
  money_de(r->total_cents, money, sizeof money);
  const char *cd = r->collection_date ? r->collection_date : "";
  if (strlen(cd) == 10 && cd[4] == '-' && cd[7] == '-') snprintf(date, sizeof date, "%.2s.%.2s.%.4s", cd + 8, cd + 5, cd);
  else snprintf(date, sizeof date, "%s", cd);
  size_t sl = 0, bl = 0;
  subject[0] = body[0] = 0;
  add(subject, scap, &sl, "Abrechnung %s versendet", label);
  add(body, bcap, &bl, "Abrechnungslauf %lld (%s)\n\nRechnungen versendet: %zu von %zu\nBei diesem Versand: %zu\nFehlgeschlagen: ",
      (long long)r->run_id, label, r->n_mailed_total, r->n_invoices, r->n_sent_now);
  if (!r->n_failed) add(body, bcap, &bl, "keine");
  for (size_t i = 0; i < r->n_failed; i++) add(body, bcap, &bl, "%s%lld", i ? ", " : "", (long long)r->failed[i]);
  add(body, bcap, &bl, "\nSumme: %s\nEinzugstermin: %s\n\n", money, date);
  if (r->n_failed) add(body, bcap, &bl, "Bitte den Versand f\xC3\xBCr die fehlgeschlagenen Rechnungen wiederholen.\n\n");
  add(body, bcap, &bl, "%s/#/admin/abrechnung\n", r->base_url);
  return sl >= scap || bl >= bcap ? -1 : 0;
}
