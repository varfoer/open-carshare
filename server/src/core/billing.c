#include "core/billing.h"
#include <stdio.h>
#include <string.h>
#include "core/berlin.h"

/* Howard Hinnant's civil-date algorithms, days since 1970-01-01. */
static int64_t days_from_civil(int64_t y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yy + (*m <= 2));
}

/* Floor division for negative-safe local day numbers. */
static int64_t local_day(int64_t utc) {
  int64_t l = utc + berlin_offset(utc);
  return l >= 0 ? l / 86400 : -((-l + 86399) / 86400);
}

int billing_period_parse(const char *s, int *year, int *month) {
  if (!s || strlen(s) != 7 || s[4] != '-') return -1;
  for (int i = 0; i < 7; i++)
    if (i != 4 && (s[i] < '0' || s[i] > '9')) return -1;
  int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  int m = (s[5] - '0') * 10 + (s[6] - '0');
  if (y < 2000 || y > 2099 || m < 1 || m > 12) return -1;
  *year = y; *month = m;
  return 0;
}

static int64_t local_month_start(int year, int month) {
  int64_t naive = days_from_civil(year, month, 1) * 86400;
  /* naive - 1h is on local day 1 under both offsets (1h, 2h) */
  return berlin_day_start(naive - 3600);
}

void billing_period_bounds(int year, int month, int64_t *from, int64_t *to) {
  *from = local_month_start(year, month);
  *to = month == 12 ? local_month_start(year + 1, 1) : local_month_start(year, month + 1);
}

int64_t billing_bill_time(const booking_t *b) {
  if (b->state == BK_DONE) return b->actual_end ? b->actual_end : b->booked_end;
  if (b->state == BK_CANCELLED) return b->cancelled_at;
  return 0;
}

bool billing_includes(const booking_t *b, int64_t period_end) {
  if (b->invoice_id != 0) return false;
  if (b->state == BK_CANCELLED) {
    if (b->cancel_fee_cents <= 0 || b->club_trip) return false;
  } else if (b->state != BK_DONE) {
    return false;
  }
  int64_t t = billing_bill_time(b);
  return t > 0 && t < period_end;
}

void billing_line(const booking_t *b, const char *car_name, const price_t *p, invoice_line_t *out) {
  memset(out, 0, sizeof *out);
  out->booking_id = b->id;
  snprintf(out->car_name, sizeof out->car_name, "%s", car_name ? car_name : "");
  out->cancelled = b->state == BK_CANCELLED;
  out->club_trip = b->club_trip;
  out->never_started = !out->cancelled && b->actual_start == 0;
  if (!out->cancelled && b->actual_start != 0) {
    out->start = b->actual_start;
    out->end = b->actual_end;
  } else {
    out->start = b->booked_start;
    out->end = b->booked_end;
  }
  out->day_min = p->day_min;
  out->night_min = p->night_min;
  out->unused_min = p->unused_day_min + p->unused_night_min;
  if (b->club_trip)
    out->km = (!b->km_pending && b->km_end > 0 && b->km_end >= b->km_start)
                  ? b->km_end - b->km_start : 0;
  else
    out->km = p->km < 0 ? 0 : p->km;
  out->time_cents = p->time_cents;
  out->km_cents = p->km_cents;
  out->cap_cut_cents = p->time_cap_cut_cents + p->combined_cap_cut_cents;
  out->late_fee_cents = p->late_fee_cents;
  out->cancel_fee_cents = p->cancel_fee_cents;
  out->total_cents = p->total_cents;
}

/* Easter Sunday as day number (anonymous Gregorian algorithm). */
static int64_t easter_day(int y) {
  int a = y % 19, b = y / 100, c = y % 100, d = b / 4, e = b % 4;
  int f = (b + 8) / 25, g = (b - f + 1) / 3;
  int h = (19 * a + b - d - g + 15) % 30;
  int i = c / 4, k = c % 4;
  int l = (32 + 2 * e + 2 * i - h - k) % 7;
  int m = (a + 11 * h + 22 * l) / 451;
  int month = (h + l - 7 * m + 114) / 31;
  int day = (h + l - 7 * m + 114) % 31 + 1;
  return days_from_civil(y, month, day);
}

bool target2_open(int year, int month, int day) {
  if (month == 1 && day == 1) return false;
  if (month == 5 && day == 1) return false;
  if (month == 12 && (day == 25 || day == 26)) return false;
  int64_t n = days_from_civil(year, month, day);
  int64_t wd = ((n % 7) + 7 + 4) % 7;  /* 1970-01-01 Thursday; 0 = Sunday */
  if (wd == 0 || wd == 6) return false;
  int64_t easter = easter_day(year);
  return n != easter - 2 && n != easter + 1;
}

void billing_collection_date(int64_t now, int lead_days, char out[11]) {
  int64_t n = local_day(now) + lead_days;
  for (;;) {
    int y, m, d;
    civil_from_days(n, &y, &m, &d);
    if (target2_open(y, m, d)) {
      snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
      return;
    }
    n++;
  }
}

void billing_local_date(int64_t utc, char out[11]) {
  int y, m, d;
  civil_from_days(local_day(utc), &y, &m, &d);
  snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}
