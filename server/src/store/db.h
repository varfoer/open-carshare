#ifndef OCS_STORE_DB_H
#define OCS_STORE_DB_H

#include <stddef.h>
#include <stdint.h>
#include "core/model.h"

typedef struct {
  char dir[256];
  user_t *users; size_t n_users, cap_users;
  car_t *cars; size_t n_cars, cap_cars;
  booking_t *bookings; size_t n_bookings, cap_bookings;
  session_t *sessions; size_t n_sessions, cap_sessions;
  token_t *tokens; size_t n_tokens, cap_tokens;
  damage_report_t *damages; size_t n_damages, cap_damages;
  tariff_t *tariffs; size_t n_tariffs, cap_tariffs;
  tariff_version_t *versions; size_t n_versions, cap_versions;
  user_tariff_t *user_tariffs; size_t n_user_tariffs, cap_user_tariffs;
  mandate_t *mandates; size_t n_mandates, cap_mandates;
  billing_run_t *runs; size_t n_runs, cap_runs;
  invoice_t *invoices; size_t n_invoices, cap_invoices;
  invoice_line_t *lines; size_t n_lines, cap_lines;
  settings_t settings;
  access_pin_t *pins; size_t n_pins, cap_pins;
  odometer_reading_t *readings; size_t n_readings, cap_readings;
  int64_t km_mail_day;
} db_t;

/* 0 ok, -1 on bad dir, unreadable or corrupt file. db is freed on failure.
 * Creates <dir>/photos, <dir>/invoices and <dir>/billing if missing. Afterwards drops (RAM
 * only) invoices and lines whose run is missing or not RUN_FINAL, then sets invoice_id = 0
 * on bookings whose invoice no longer exists. */
int  db_load(db_t *db, const char *dir);
void db_free(db_t *db);

user_t *db_user(db_t *db, int64_t id);
user_t *db_user_by_email(db_t *db, const char *email); /* ASCII case-insensitive */
car_t *db_car(db_t *db, int64_t id);
car_t *db_car_by_imei(db_t *db, const char *imei); /* NULL for "" or no match; any car, active or not */
car_t *db_car_by_lock(db_t *db, const char *device_id); /* NULL for "" or no match; any car, active or not */
booking_t *db_booking(db_t *db, int64_t id);
damage_report_t *db_damage(db_t *db, int64_t id);
token_t *db_token(db_t *db, const char *token_hash);
tariff_t *db_tariff(db_t *db, int64_t id);
tariff_t *db_tariff_by_name(db_t *db, const char *name); /* exact match; NULL if none */
tariff_version_t *db_version(db_t *db, int64_t id);
user_tariff_t *db_user_tariff(db_t *db, int64_t id);
mandate_t *db_mandate(db_t *db, int64_t id);
const mandate_t *db_mandate_current(const db_t *db, int64_t user_id); /* status ACTIVE, largest id; NULL if none */
billing_run_t *db_run(db_t *db, int64_t id);
invoice_t *db_invoice(db_t *db, int64_t number);
int64_t db_next_invoice_number(const db_t *db);      /* max number + 1 */
/* Version of the tariff with the largest valid_from <= ts; NULL if none. */
const tariff_version_t *db_version_at(const db_t *db, int64_t tariff_id, int64_t ts);
/* Assignment of the user with the largest valid_from <= ts; NULL if none. */
const user_tariff_t *db_user_tariff_at(const db_t *db, int64_t user_id, int64_t ts);
/* Assignment of the user with the largest valid_from; NULL if none. */
const user_tariff_t *db_user_tariff_newest(const db_t *db, int64_t user_id);
/* Price list in force for the user at ts: the assignment at ts, then its tariff's version
 * at ts. NULL if either is missing. */
const tariff_version_t *db_tariff_at(const db_t *db, int64_t user_id, int64_t ts);

/* All return 0 on success, -1 if persisting failed; RAM is then restored to its
 * state before the call, including the id counter. */
int db_put_user(db_t *db, user_t *u);  /* id==0: assign max+1 and insert; else replace */
int db_put_car(db_t *db, car_t *c);
int db_put_booking(db_t *db, booking_t *b);
int db_put_damage(db_t *db, damage_report_t *d);
int db_put_tariff(db_t *db, tariff_t *t);      /* id==0: assign max+1 and insert; else replace */
int db_put_version(db_t *db, tariff_version_t *v); /* id==0: assign max+1 and insert; else replace */
int db_put_user_tariff(db_t *db, user_tariff_t *u); /* id==0: assign max+1 and insert; else replace */
int db_put_mandate(db_t *db, mandate_t *m);   /* id==0: assign max+1 and insert; else replace */
/* Delete by id; -1 if unknown id or persisting failed. db_delete_tariff also removes the
 * tariff's versions (tariff_versions.jsonl is written first). */
int db_delete_tariff(db_t *db, int64_t id);
int db_delete_version(db_t *db, int64_t id);
int db_delete_user_tariff(db_t *db, int64_t id);
int db_put_run(db_t *db, billing_run_t *r);   /* id==0: assign max+1 and insert; else replace */
int db_delete_run(db_t *db, int64_t id);      /* -1 if unknown id or persisting failed */
int db_put_invoices(db_t *db, const invoice_t *inv, size_t n); /* replace by number, one write */
/* Inserts invoices and lines, sets invoice_id = line.invoice_number on every line's booking,
 * then replaces the run. Writes invoice_lines.jsonl, invoices.jsonl, bookings.jsonl,
 * billing_runs.jsonl in this order, so a crash before the last leaves the run DRAFT and load
 * drops the orphans. RAM restored on -1. */
int db_finalize_run(db_t *db, const billing_run_t *run, const invoice_t *inv, size_t n_inv,
                    const invoice_line_t *lines, size_t n_lines);
int db_put_settings(db_t *db, const settings_t *s);
int db_put_session(db_t *db, const session_t *s);
int db_delete_sessions(db_t *db, const char *token_hash, int64_t user_id); /* by hash if non-NULL, else all of user */
int db_put_token(db_t *db, const token_t *t);  /* insert or replace by token_hash */
int db_prune(db_t *db, int64_t now);           /* drop expired sessions/tokens, persist */

/* Append-only odometer readings. add: all n lines go out in one append; -1 means
 * nothing was added. prune: drops ts < before, rewrites the file; RAM restored on -1. */
int db_add_readings(db_t *db, const odometer_reading_t *r, size_t n);
int db_prune_readings(db_t *db, int64_t before);
/* access_pins.jsonl: normal table (missing file = empty). put: id==0 -> max+1 insert, else
 * replace or insert; RAM restored on -1. newest: row of the booking with the largest id, NULL
 * if none; only meaningful for booking_id > 0 (test PINs use 0). max_variance: highest variance
 * of rows for the car with that valid_from, 0 if none. blank: sets pin "" on all rows of the
 * booking in one write, no write if nothing changes. prune: drops created_at < before.
 * blank/prune restore RAM on -1. Pointers into the table are invalid after any put. */
int db_put_pin(db_t *db, access_pin_t *p);
const access_pin_t *db_pin_newest(const db_t *db, int64_t booking_id);
int db_pin_max_variance(const db_t *db, int64_t car_id, int64_t valid_from);
int db_pins_blank(db_t *db, int64_t booking_id);
int db_prune_pins(db_t *db, int64_t before);
/* Persists jobs.json {"km_mail_day":N}, then sets db->km_mail_day. */
int db_put_km_mail_day(db_t *db, int64_t day);

/* Writes "<dir>/photos/r<report_id>-<k>.jpg" (k is 1-based) into out. -1 if it does not fit. */
int db_photo_path(const db_t *db, int64_t report_id, int k, char *out, size_t cap);

/* "<dir>/invoices/<number>.pdf" and "billing/run-<id>-pain008.xml" (relative to dir).
 * -1 if it does not fit. */
int db_invoice_pdf_path(const db_t *db, int64_t number, char *out, size_t cap);
int db_run_xml_relpath(int64_t run_id, char *out, size_t cap);

#endif
