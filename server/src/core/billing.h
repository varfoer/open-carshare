#ifndef OCS_CORE_BILLING_H
#define OCS_CORE_BILLING_H

#include <stdbool.h>
#include <stdint.h>
#include "core/model.h"
#include "core/pricing.h"

/* "YYYY-MM", year 2000..2099, month 01..12; 0 ok, -1 invalid. */
int billing_period_parse(const char *s, int *year, int *month);
/* UTC instants of the local (Europe/Berlin) starts of the month and the next month. */
void billing_period_bounds(int year, int month, int64_t *from, int64_t *to);
/* DONE: actual_end or booked_end; CANCELLED: cancelled_at; else 0. */
int64_t billing_bill_time(const booking_t *b);
/* invoice_id == 0, bill time in (0, period_end), and DONE, or CANCELLED with
 * cancel_fee_cents > 0 && !club_trip. */
bool billing_includes(const booking_t *b, int64_t period_end);
/* car_name is copied (never truncated: callers pass car_t.name, which has the same size). invoice_number 0. */
void billing_line(const booking_t *b, const char *car_name, const price_t *p, invoice_line_t *out);
/* TARGET2 open day: not Sat/Sun, 1 Jan, Good Friday, Easter Monday, 1 May, 25/26 Dec. */
bool target2_open(int year, int month, int day);
/* Local date of now + lead_days, moved forward to a TARGET2 day; writes "YYYY-MM-DD". */
void billing_collection_date(int64_t now, int lead_days, char out[11]);
/* Local (Berlin) date of now + days calendar days, no TARGET2 shift; writes "YYYY-MM-DD". */
void billing_local_date_plus(int64_t now, int days, char out[11]);
/* "YYYY-MM-DD", Berlin. */
void billing_local_date(int64_t utc, char out[11]);

#endif
