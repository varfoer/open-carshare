#include "store/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core/codec.h"
#include "store/store.h"

typedef cJSON *(*to_json_fn)(const void *);

/* ---- helpers ---- */

static int table_path(const db_t *db, const char *file, char *out, size_t cap) {
  int k = snprintf(out, cap, "%s/%s", db->dir, file);
  return (k < 0 || (size_t)k >= cap) ? -1 : 0;
}

static int buf_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
  if (*len + n > *cap) {
    size_t nc = *cap ? *cap : 256;
    while (nc < *len + n) nc *= 2;
    char *nb = realloc(*buf, nc);
    if (!nb) return -1;
    *buf = nb;
    *cap = nc;
  }
  memcpy(*buf + *len, s, n);
  *len += n;
  return 0;
}

/* Serializes one table as JSONL and writes it atomically. */
static int persist_table(const db_t *db, const char *file, const void *arr, size_t n,
                         size_t esz, to_json_fn to_json) {
  char path[512];
  if (table_path(db, file, path, sizeof path) != 0) return -1;
  char *buf = NULL;
  size_t len = 0, cap = 0;
  int rc = 0;
  for (size_t i = 0; i < n && rc == 0; i++) {
    cJSON *j = to_json((const char *)arr + i * esz);
    char *s = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!s || buf_append(&buf, &len, &cap, s, strlen(s)) != 0 ||
        buf_append(&buf, &len, &cap, "\n", 1) != 0)
      rc = -1;
    free(s);
  }
  if (rc == 0) rc = store_write_atomic(path, buf ? buf : "", len);
  free(buf);
  return rc;
}

static int grow(void **arr, size_t *cap, size_t need, size_t esz) {
  if (need <= *cap) return 0;
  size_t nc = *cap ? *cap * 2 : 8;
  while (nc < need) nc *= 2;
  void *p = realloc(*arr, nc * esz);
  if (!p) return -1;
  *arr = p;
  *cap = nc;
  return 0;
}

/* Insert at index n, or replace element idx (idx < 0: insert). Persists; restores on failure. */
static int put_at(const db_t *db, void **arr, size_t *n, size_t *cap, size_t esz, long idx,
                  const void *elem, const char *file, to_json_fn to_json) {
  char *base;
  if (idx >= 0) {
    char *old = malloc(esz);
    if (!old) return -1;
    base = *arr;
    memcpy(old, base + (size_t)idx * esz, esz);
    memcpy(base + (size_t)idx * esz, elem, esz);
    int rc = persist_table(db, file, *arr, *n, esz, to_json);
    if (rc != 0) memcpy(base + (size_t)idx * esz, old, esz);
    free(old);
    return rc;
  }
  if (grow(arr, cap, *n + 1, esz) != 0) return -1;
  base = *arr;
  memcpy(base + *n * esz, elem, esz);
  (*n)++;
  if (persist_table(db, file, *arr, *n, esz, to_json) != 0) {
    (*n)--;
    return -1;
  }
  return 0;
}

/* ---- load ---- */

#define DEFINE_LOADER_X(name, type, arr, n, cap, from_json)                    \
  static int load_##name(const cJSON *obj, void *ctx) {                        \
    db_t *db = ctx;                                                            \
    type v;                                                                    \
    if (from_json(obj, &v) != 0) return -1;                                    \
    if (grow((void **)&db->arr, &db->cap_##name, db->n_##name + 1, sizeof v) != 0) return -1; \
    db->arr[db->n_##name++] = v;                                               \
    return 0;                                                                  \
  }

DEFINE_LOADER_X(users, user_t, users, n_users, cap_users, user_from_json)
DEFINE_LOADER_X(cars, car_t, cars, n_cars, cap_cars, car_from_json)
DEFINE_LOADER_X(bookings, booking_t, bookings, n_bookings, cap_bookings, booking_from_json)
DEFINE_LOADER_X(sessions, session_t, sessions, n_sessions, cap_sessions, session_from_json)
DEFINE_LOADER_X(tokens, token_t, tokens, n_tokens, cap_tokens, token_from_json)

static int load_settings(const cJSON *obj, void *ctx) {
  db_t *db = ctx;
  return settings_from_json(obj, &db->settings);
}

static int load_file(db_t *db, const char *file, int (*cb)(const cJSON *, void *)) {
  char path[512];
  if (table_path(db, file, path, sizeof path) != 0) return -1;
  return store_read_lines(path, cb, db);
}

void db_free(db_t *db) {
  free(db->users);
  free(db->cars);
  free(db->bookings);
  free(db->sessions);
  free(db->tokens);
  memset(db, 0, sizeof *db);
}

int db_load(db_t *db, const char *dir) {
  memset(db, 0, sizeof *db);
  int k = snprintf(db->dir, sizeof db->dir, "%s", dir);
  if (k < 0 || (size_t)k >= sizeof db->dir) {
    memset(db, 0, sizeof *db);
    return -1;
  }
  cJSON *empty = cJSON_CreateObject();
  int rc = empty ? settings_from_json(empty, &db->settings) : -1; /* defaults */
  cJSON_Delete(empty);
  if (rc == 0) rc = load_file(db, "users.jsonl", load_users);
  if (rc == 0) rc = load_file(db, "cars.jsonl", load_cars);
  if (rc == 0) rc = load_file(db, "bookings.jsonl", load_bookings);
  if (rc == 0) rc = load_file(db, "sessions.jsonl", load_sessions);
  if (rc == 0) rc = load_file(db, "tokens.jsonl", load_tokens);
  if (rc == 0) rc = load_file(db, "settings.json", load_settings);
  if (rc != 0) {
    db_free(db);
    return -1;
  }
  return 0;
}

/* ---- lookups ---- */

user_t *db_user(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_users; i++)
    if (db->users[i].id == id) return &db->users[i];
  return NULL;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static int ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (lower(*a) != lower(*b)) return 0;
  return *a == *b;
}

user_t *db_user_by_email(db_t *db, const char *email) {
  for (size_t i = 0; i < db->n_users; i++)
    if (ieq(db->users[i].email, email)) return &db->users[i];
  return NULL;
}

car_t *db_car(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_cars; i++)
    if (db->cars[i].id == id) return &db->cars[i];
  return NULL;
}

booking_t *db_booking(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_bookings; i++)
    if (db->bookings[i].id == id) return &db->bookings[i];
  return NULL;
}

token_t *db_token(db_t *db, const char *token_hash) {
  for (size_t i = 0; i < db->n_tokens; i++)
    if (strcmp(db->tokens[i].token_hash, token_hash) == 0) return &db->tokens[i];
  return NULL;
}

/* ---- writes ---- */

/* Entity tables share the id logic: id==0 -> max+1 insert, else replace or insert. */
#define DEFINE_PUT_ENTITY(fn, type, arr, n, cap, file, to_json)                          \
  int fn(db_t *db, type *e) {                                                            \
    long idx = -1;                                                                       \
    int64_t max = 0, orig = e->id;                                                       \
    for (size_t i = 0; i < db->n; i++) {                                                 \
      if (db->arr[i].id > max) max = db->arr[i].id;                                      \
      if (orig != 0 && db->arr[i].id == orig) idx = (long)i;                             \
    }                                                                                    \
    if (orig == 0) e->id = max + 1;                                                      \
    if (put_at(db, (void **)&db->arr, &db->n, &db->cap, sizeof *e, idx, e, file,         \
               (to_json_fn)to_json) != 0) {                                              \
      e->id = orig;                                                                      \
      return -1;                                                                         \
    }                                                                                    \
    return 0;                                                                            \
  }

DEFINE_PUT_ENTITY(db_put_user, user_t, users, n_users, cap_users, "users.jsonl", user_to_json)
DEFINE_PUT_ENTITY(db_put_car, car_t, cars, n_cars, cap_cars, "cars.jsonl", car_to_json)
DEFINE_PUT_ENTITY(db_put_booking, booking_t, bookings, n_bookings, cap_bookings,
                  "bookings.jsonl", booking_to_json)

int db_put_settings(db_t *db, const settings_t *s) {
  char path[512];
  if (table_path(db, "settings.json", path, sizeof path) != 0) return -1;
  cJSON *j = settings_to_json(s);
  char *str = j ? cJSON_PrintUnformatted(j) : NULL;
  cJSON_Delete(j);
  if (!str) return -1;
  size_t n = strlen(str);
  char *line = malloc(n + 2);
  if (!line) {
    free(str);
    return -1;
  }
  memcpy(line, str, n);
  line[n] = '\n';
  int rc = store_write_atomic(path, line, n + 1);
  free(line);
  free(str);
  if (rc != 0) return -1;
  db->settings = *s;
  return 0;
}

int db_put_session(db_t *db, const session_t *s) {
  long idx = -1;
  for (size_t i = 0; i < db->n_sessions; i++)
    if (strcmp(db->sessions[i].token_hash, s->token_hash) == 0) idx = (long)i;
  return put_at(db, (void **)&db->sessions, &db->n_sessions, &db->cap_sessions, sizeof *s, idx,
                s, "sessions.jsonl", (to_json_fn)session_to_json);
}

int db_put_token(db_t *db, const token_t *t) {
  long idx = -1;
  for (size_t i = 0; i < db->n_tokens; i++)
    if (strcmp(db->tokens[i].token_hash, t->token_hash) == 0) idx = (long)i;
  return put_at(db, (void **)&db->tokens, &db->n_tokens, &db->cap_tokens, sizeof *t, idx, t,
                "tokens.jsonl", (to_json_fn)token_to_json);
}

/* Removes sessions: expired (use_now), else by hash if non-NULL, else by user_id.
 * Restores RAM if persisting fails. */
static int sessions_remove(db_t *db, const char *hash, int64_t user_id, int64_t now,
                           int use_now) {
  size_t n = db->n_sessions;
  session_t *copy = n ? malloc(n * sizeof *copy) : NULL;
  if (n && !copy) return -1;
  if (n) memcpy(copy, db->sessions, n * sizeof *copy);
  size_t w = 0;
  for (size_t i = 0; i < n; i++) {
    const session_t *s = &copy[i];
    int drop = use_now ? s->expires_at <= now
                       : (hash ? strcmp(s->token_hash, hash) == 0 : s->user_id == user_id);
    if (!drop) db->sessions[w++] = *s;
  }
  db->n_sessions = w;
  int rc = 0;
  if (w != n)
    rc = persist_table(db, "sessions.jsonl", db->sessions, w, sizeof *copy,
                       (to_json_fn)session_to_json);
  if (rc != 0) {
    memcpy(db->sessions, copy, n * sizeof *copy);
    db->n_sessions = n;
  }
  free(copy);
  return rc == 0 ? 0 : -1;
}

int db_delete_sessions(db_t *db, const char *token_hash, int64_t user_id) {
  return sessions_remove(db, token_hash, user_id, 0, 0);
}

int db_prune(db_t *db, int64_t now) {
  if (sessions_remove(db, NULL, 0, now, 1) != 0) return -1;
  size_t n = db->n_tokens;
  token_t *copy = n ? malloc(n * sizeof *copy) : NULL;
  if (n && !copy) return -1;
  if (n) memcpy(copy, db->tokens, n * sizeof *copy);
  size_t w = 0;
  for (size_t i = 0; i < n; i++)
    if (copy[i].expires_at > now) db->tokens[w++] = copy[i];
  db->n_tokens = w;
  int rc = 0;
  if (w != n)
    rc = persist_table(db, "tokens.jsonl", db->tokens, w, sizeof *copy, (to_json_fn)token_to_json);
  if (rc != 0) {
    memcpy(db->tokens, copy, n * sizeof *copy);
    db->n_tokens = n;
  }
  free(copy);
  return rc == 0 ? 0 : -1;
}
