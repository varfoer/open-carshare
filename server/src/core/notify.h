#ifndef OCS_CORE_NOTIFY_H
#define OCS_CORE_NOTIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/model.h"

/* Pure mail texts for booking events, reminders and the billing run (spec §10). Bodies use LF;
 * mail_build converts to CRLF. */

typedef enum { NT_CREATED = 0, NT_MODIFIED, NT_CANCELLED, NT_REMIND_START, NT_REMIND_LATE, NT_REMIND_END } notify_kind_t;
typedef struct {
  notify_kind_t kind;
  const booking_t *b; const car_t *car;
  const char *user_name, *base_url;
  bool by_admin;                 /* NT_MODIFIED/NT_CANCELLED: actor != booking user */
  int start_tolerance_min;       /* NT_REMIND_START: start allowed from; NT_REMIND_LATE/END: fee from */
  int64_t late_fee_cents_per_h;  /* NT_REMIND_LATE/END; 0 = no fee line */
} notify_booking_t;
/* 0 ok, -1 if subject or body does not fit. */
int notify_booking_mail(const notify_booking_t *n, char *subject, size_t scap, char *body, size_t bcap);
/* Short push texts for NT_REMIND_START/END/LATE. 0 ok, -1 for other kinds or if a text does not fit. */
int notify_booking_push(const notify_booking_t *n, char *title, size_t tcap, char *body, size_t bcap);

typedef struct {
  int64_t run_id; const char *period;          /* "YYYY-MM" */
  size_t n_invoices, n_mailed_total, n_sent_now;
  const int64_t *failed; size_t n_failed;
  int64_t total_cents; const char *collection_date;  /* "YYYY-MM-DD" */
  const char *base_url;
} notify_run_t;
/* 0 ok, -1 if subject or body does not fit. */
int notify_run_mail(const notify_run_t *r, char *subject, size_t scap, char *body, size_t bcap);

void notify_local_time(int64_t utc, char out[17]);   /* "04.11.2026 14:30", Berlin */

#endif
