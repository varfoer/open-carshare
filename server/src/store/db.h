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
  settings_t settings;
} db_t;

/* 0 ok, -1 on bad dir, unreadable or corrupt file. db is freed on failure. */
int  db_load(db_t *db, const char *dir);
void db_free(db_t *db);

user_t *db_user(db_t *db, int64_t id);
user_t *db_user_by_email(db_t *db, const char *email); /* ASCII case-insensitive */
car_t *db_car(db_t *db, int64_t id);
booking_t *db_booking(db_t *db, int64_t id);
token_t *db_token(db_t *db, const char *token_hash);

/* All return 0 on success, -1 if persisting failed; RAM is then restored to its
 * state before the call, including the id counter. */
int db_put_user(db_t *db, user_t *u);  /* id==0: assign max+1 and insert; else replace */
int db_put_car(db_t *db, car_t *c);
int db_put_booking(db_t *db, booking_t *b);
int db_put_settings(db_t *db, const settings_t *s);
int db_put_session(db_t *db, const session_t *s);
int db_delete_sessions(db_t *db, const char *token_hash, int64_t user_id); /* by hash if non-NULL, else all of user */
int db_put_token(db_t *db, const token_t *t);  /* insert or replace by token_hash */
int db_prune(db_t *db, int64_t now);           /* drop expired sessions/tokens, persist */

#endif
