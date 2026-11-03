#include "core/invoice_doc.h"

#include <stdio.h>
#include <string.h>
#include "core/berlin.h"
#include "core/billing.h"
#include "core/pdf.h"

#define LEFT 50
#define RIGHT 555
#define MAX_LINES 24
#define LINE_CAP 256

void money_de(int64_t cents, char *out, size_t cap) {
  uint64_t a = cents < 0 ? 0 - (uint64_t)cents : (uint64_t)cents;
  unsigned long long whole = a / 100, frac = a % 100;
  char digits[24], grouped[32];
  int nd = snprintf(digits, sizeof digits, "%llu", whole), g = 0;
  for (int i = 0; i < nd; i++) {
    if (i > 0 && (nd - i) % 3 == 0) grouped[g++] = '.';
    grouped[g++] = digits[i];
  }
  grouped[g] = 0;
  snprintf(out, cap, "%s%s,%02llu \xE2\x82\xAC", cents < 0 ? "-" : "", grouped, frac);
}

/* "2026-10" -> "Oktober 2026"; anything unparsable is returned as is. */
static void period_label(const char *period, char *out, size_t cap) {
  static const char *const months[12] = {"Januar", "Februar", "März", "April", "Mai", "Juni", "Juli",
                                         "August", "September", "Oktober", "November", "Dezember"};
  int y, m;
  if (billing_period_parse(period, &y, &m) == 0) snprintf(out, cap, "%s %d", months[m - 1], y);
  else snprintf(out, cap, "%s", period);
}

void invoice_subject(const invoice_t *inv, char *out, size_t cap) {
  char label[40];
  period_label(inv->period, label, sizeof label);
  snprintf(out, cap, "Rechnung %lld \xE2\x80\x93 %s", (long long)inv->number, label);
}

/* "YYYY-MM-DD" -> "dd.mm.yyyy"; -1 if malformed. */
static int date_de(const char *iso, char out[11]) {
  if (!iso || strlen(iso) != 10 || iso[4] != '-' || iso[7] != '-') return -1;
  snprintf(out, 11, "%.2s.%.2s.%.4s", iso + 8, iso + 5, iso);
  return 0;
}

static int prenotification(const invoice_doc_t *d, char *out, size_t cap) {
  if (d->inv->total_cents <= 0) {
    int n = snprintf(out, cap, "Es wird kein Betrag eingezogen.");
    return n < 0 || (size_t)n >= cap ? -1 : n;
  }
  char date[11], amount[32];
  if (!d->mandate_ref || !d->iban_masked || date_de(d->collection_date, date) < 0) return -1;
  money_de(d->inv->total_cents, amount, sizeof amount);
  int n = snprintf(out, cap,
                   "Wir ziehen den Gesamtbetrag von %s am %s per SEPA-Lastschrift von deinem Konto %s ein. "
                   "Mandatsreferenz: %s, Gläubiger-ID: %s.",
                   amount, date, d->iban_masked, d->mandate_ref, d->s->creditor_id);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

int invoice_mail_body(const invoice_doc_t *d, const char *name, char *out, size_t cap) {
  char pre[600], label[40];
  if (prenotification(d, pre, sizeof pre) < 0) return -1;
  period_label(d->inv->period, label, sizeof label);
  int n = snprintf(out, cap, "Hallo %s,\n\nanbei deine Rechnung %lld für %s.\n\n%s\n\nViele Grüße\n%s\n",
                   name, (long long)d->inv->number, label, pre, d->s->assoc_name);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* Greedy word wrap of one paragraph into lines[*n..]; -1 if a word or the line count does not fit. */
static int wrap(int font, int size, int maxw, const char *text, char lines[][LINE_CAP], int *n) {
  char cur[LINE_CAP] = "";
  const char *s = text;
  for (;;) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char *e = s;
    while (*e && *e != ' ') e++;
    size_t wl = (size_t)(e - s);
    if (wl >= LINE_CAP) return -1;
    char trial[2 * LINE_CAP];
    snprintf(trial, sizeof trial, "%s%s%.*s", cur, cur[0] ? " " : "", (int)wl, s);
    if (cur[0] && (strlen(trial) >= LINE_CAP || pdf_text_width(font, size, trial) > maxw)) {
      if (*n >= MAX_LINES) return -1;
      memcpy(lines[(*n)++], cur, strlen(cur) + 1);
      snprintf(cur, sizeof cur, "%.*s", (int)wl, s);
    } else {
      memcpy(cur, trial, strlen(trial) + 1);
    }
    s = e;
  }
  if (*n >= MAX_LINES) return -1;
  memcpy(lines[(*n)++], cur, strlen(cur) + 1);
  return 0;
}

static void hours(int64_t min, char *out, size_t cap) {
  snprintf(out, cap, "%lld,%lld", (long long)(min / 60), (long long)((min % 60) * 10 / 60));
}

static void local_hm(int64_t utc, char out[6]) {
  int64_t t = (utc + berlin_offset(utc)) % 86400;
  snprintf(out, 6, "%02d:%02d", (int)(t / 3600), (int)(t % 3600 / 60));
}

static void col_header(pdf_t *p, int y) {
  static const struct { const char *t; int x; int right; } cols[] = {
    {"Datum", LEFT, 0}, {"Zeit", 92, 0}, {"Auto", 152, 0}, {"Tag h", 300, 1}, {"Nacht h", 345, 1},
    {"Ungenutzt h", 405, 1}, {"km", 445, 1}, {"Gebühren", 500, 1}, {"Betrag", RIGHT, 1}};
  for (size_t i = 0; i < sizeof cols / sizeof *cols; i++) {
    if (cols[i].right) pdf_text_right(p, 1, 8, cols[i].x, y, cols[i].t);
    else pdf_text(p, 1, 8, cols[i].x, y, cols[i].t);
  }
  pdf_hline(p, LEFT, RIGHT, y - 4);
}

/* Car name clipped with ".." (visible) to the Auto column. */
static void car_cell(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s", name);
  if (pdf_text_width(0, 9, out) <= 118) return;
  size_t n = strlen(out);
  while (n > 0) {
    n--;
    while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;   /* stay on a UTF-8 boundary */
    snprintf(out + n, cap - n, "..");
    if (pdf_text_width(0, 9, out) <= 118) return;
  }
}

static void line_note(const invoice_line_t *l, char *out, size_t cap) {
  char cut[40], part[64];
  out[0] = 0;
  if (l->club_trip) snprintf(out + strlen(out), cap - strlen(out), "Vereinsfahrt");
  if (l->cancelled) snprintf(out + strlen(out), cap - strlen(out), "%sStorniert", out[0] ? ", " : "");
  else if (l->never_started) snprintf(out + strlen(out), cap - strlen(out), "%sNicht angetreten", out[0] ? ", " : "");
  if (l->cap_cut_cents > 0) {
    money_de(l->cap_cut_cents, cut, sizeof cut);
    snprintf(part, sizeof part, "Deckelung -%s", cut);
    snprintf(out + strlen(out), cap - strlen(out), "%s%s", out[0] ? ", " : "", part);
  }
}

/* Footer: split at '\n' (blank segments stay as blank lines), each wrapped to the page width. */
static int wrap_footer(const char *footer, char lines[][LINE_CAP], int *n) {
  for (const char *s = footer; *s;) {
    const char *e = strchr(s, '\n');
    size_t sl = e ? (size_t)(e - s) : strlen(s);
    char seg[sizeof ((settings_t *)0)->invoice_footer];
    if (sl >= sizeof seg) return -1;
    memcpy(seg, s, sl); seg[sl] = 0;
    if (wrap(0, 8, RIGHT - LEFT, seg, lines, n) < 0) return -1;
    s += sl + (e ? 1 : 0);
  }
  return 0;
}

bool invoice_footer_fits(const char *footer) {
  char fl[MAX_LINES][LINE_CAP];
  int n = 0;
  return wrap_footer(footer, fl, &n) == 0;
}

int invoice_pdf(const invoice_doc_t *d, char **out, size_t *len) {
  const invoice_t *inv = d->inv;
  char pre[600], label[40], buf[400], a[64], b[64];
  char pl[MAX_LINES][LINE_CAP], fl[MAX_LINES][LINE_CAP];
  int npl = 0, nfl = 0;
  if (prenotification(d, pre, sizeof pre) < 0) return -1;
  if (wrap(0, 10, RIGHT - LEFT, pre, pl, &npl) < 0) return -1;
  if (wrap_footer(d->s->invoice_footer, fl, &nfl) < 0) return -1;
  int footer_top = 60 + 10 * (nfl > 0 ? nfl - 1 : 0);
  int limit = footer_top + 20 > 120 ? footer_top + 20 : 120;

  pdf_t *p = pdf_new();
  if (!p) return -1;
  period_label(inv->period, label, sizeof label);
  int page = 1, y;
  pdf_page(p);

  snprintf(buf, sizeof buf, "%s \xC2\xB7 %s \xC2\xB7 %s %s", d->s->assoc_name, d->s->assoc_street, d->s->assoc_zip, d->s->assoc_city);
  pdf_text(p, 0, 8, LEFT, 760, buf);
  y = 720;
  pdf_text(p, 0, 11, LEFT, y, inv->name); y -= 14;
  if (inv->street[0]) { pdf_text(p, 0, 11, LEFT, y, inv->street); y -= 14; }
  snprintf(buf, sizeof buf, "%s %s", inv->zip, inv->city);
  pdf_text(p, 0, 11, LEFT, y, buf);

  char date[11], iso[11];
  billing_local_date(inv->issued_at ? inv->issued_at : inv->created_at, iso);
  date_de(iso, date);
  pdf_text(p, 1, 16, 400, 720, "Rechnung");
  snprintf(buf, sizeof buf, "Nr. %lld", (long long)inv->number);
  pdf_text(p, 0, 11, 400, 700, buf);
  snprintf(buf, sizeof buf, "Datum: %s", date);
  pdf_text(p, 0, 11, 400, 686, buf);
  snprintf(buf, sizeof buf, "Zeitraum: %s", label);
  pdf_text(p, 0, 11, 400, 672, buf);

  y = 600;
  col_header(p, y);
  y -= 18;
  for (size_t i = 0; i < d->n_lines; i++) {
    const invoice_line_t *l = &d->lines[i];
    char note[128];
    line_note(l, note, sizeof note);
    if (y < limit) {
      pdf_page(p); page++;
      snprintf(buf, sizeof buf, "Rechnung %lld \xE2\x80\x93 Seite %d", (long long)inv->number, page);
      pdf_text(p, 0, 8, LEFT, 810, buf);
      col_header(p, 790);
      y = 772;
    }
    billing_local_date(l->start, iso);
    snprintf(buf, sizeof buf, "%.2s.%.2s.", iso + 8, iso + 5);
    pdf_text(p, 0, 9, LEFT, y, buf);
    local_hm(l->start, a); local_hm(l->end, b);
    snprintf(buf, sizeof buf, "%s\xE2\x80\x93%s", a, b);
    pdf_text(p, 0, 9, 92, y, buf);
    car_cell(buf, sizeof buf, l->car_name);
    pdf_text(p, 0, 9, 152, y, buf);
    hours(l->day_min, buf, sizeof buf); pdf_text_right(p, 0, 9, 300, y, buf);
    hours(l->night_min, buf, sizeof buf); pdf_text_right(p, 0, 9, 345, y, buf);
    hours(l->unused_min, buf, sizeof buf); pdf_text_right(p, 0, 9, 405, y, buf);
    snprintf(buf, sizeof buf, "%lld", (long long)l->km); pdf_text_right(p, 0, 9, 445, y, buf);
    money_de(l->late_fee_cents + l->cancel_fee_cents, buf, sizeof buf); pdf_text_right(p, 0, 9, 500, y, buf);
    money_de(l->total_cents, buf, sizeof buf); pdf_text_right(p, 0, 9, RIGHT, y, buf);
    y -= 14;
    if (note[0]) { pdf_text(p, 0, 8, LEFT, y + 2, note); y -= 10; }
  }

  int need = 4 + 16 + 26 + 13 * (npl - 1);
  if (y - need < footer_top + 14) {
    pdf_page(p); page++;
    snprintf(buf, sizeof buf, "Rechnung %lld \xE2\x80\x93 Seite %d", (long long)inv->number, page);
    pdf_text(p, 0, 8, LEFT, 810, buf);
    y = 772;
  }
  y -= 4;
  pdf_hline(p, LEFT, RIGHT, y);
  y -= 16;
  pdf_text(p, 1, 11, LEFT, y, "Gesamtbetrag");
  money_de(inv->total_cents, buf, sizeof buf);
  pdf_text_right(p, 1, 11, RIGHT, y, buf);
  y -= 26;
  for (int i = 0; i < npl; i++, y -= 13) pdf_text(p, 0, 10, LEFT, y, pl[i]);
  for (int i = 0; i < nfl; i++) pdf_text(p, 0, 8, LEFT, footer_top - 10 * i, fl[i]);
  return pdf_finish(p, out, len);
}
