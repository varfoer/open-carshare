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
  mandate_t *mandates; size_t n_mandates, cap_mandates;
  settings_t settings;
  odometer_reading_t *readings; size_t n_readings, cap_readings;
  int64_t km_mail_day;
} db_t;

/* 0 ok, -1 on bad dir, unreadable or corrupt file. db is freed on failure.
 * Creates <dir>/photos if missing. */
int  db_load(db_t *db, const char *dir);
void db_free(db_t *db);

user_t *db_user(db_t *db, int64_t id);
user_t *db_user_by_email(db_t *db, const char *email); /* ASCII case-insensitive */
car_t *db_car(db_t *db, int64_t id);
car_t *db_car_by_imei(db_t *db, const char *imei); /* NULL for "" or no match; any car, active or not */
booking_t *db_booking(db_t *db, int64_t id);
damage_report_t *db_damage(db_t *db, int64_t id);
token_t *db_token(db_t *db, const char *token_hash);
tariff_t *db_tariff(db_t *db, int64_t id);
mandate_t *db_mandate(db_t *db, int64_t id);
const mandate_t *db_mandate_current(const db_t *db, int64_t user_id); /* status ACTIVE, largest id; NULL if none */
/* Version of the car with the largest valid_from <= ts; NULL if none. */
const tariff_t *db_tariff_at(const db_t *db, int64_t car_id, int64_t ts);

/* All return 0 on success, -1 if persisting failed; RAM is then restored to its
 * state before the call, including the id counter. */
int db_put_user(db_t *db, user_t *u);  /* id==0: assign max+1 and insert; else replace */
int db_put_car(db_t *db, car_t *c);
int db_put_booking(db_t *db, booking_t *b);
int db_put_damage(db_t *db, damage_report_t *d);
int db_put_tariff(db_t *db, tariff_t *t);      /* id==0: assign max+1 and insert; else replace */
int db_put_mandate(db_t *db, mandate_t *m);   /* id==0: assign max+1 and insert; else replace */
int db_delete_tariff(db_t *db, int64_t id);    /* -1 if unknown id or persisting failed */
int db_put_settings(db_t *db, const settings_t *s);
int db_put_session(db_t *db, const session_t *s);
int db_delete_sessions(db_t *db, const char *token_hash, int64_t user_id); /* by hash if non-NULL, else all of user */
int db_put_token(db_t *db, const token_t *t);  /* insert or replace by token_hash */
int db_prune(db_t *db, int64_t now);           /* drop expired sessions/tokens, persist */

/* Append-only odometer readings. add: all n lines go out in one append; -1 means
 * nothing was added. prune: drops ts < before, rewrites the file; RAM restored on -1. */
int db_add_readings(db_t *db, const odometer_reading_t *r, size_t n);
int db_prune_readings(db_t *db, int64_t before);
/* Persists jobs.json {"km_mail_day":N}, then sets db->km_mail_day. */
int db_put_km_mail_day(db_t *db, int64_t day);

/* Writes "<dir>/photos/r<report_id>-<k>.jpg" (k is 1-based) into out. -1 if it does not fit. */
int db_photo_path(const db_t *db, int64_t report_id, int k, char *out, size_t cap);

#endif
