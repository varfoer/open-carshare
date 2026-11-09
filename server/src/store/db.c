#include "store/db.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
DEFINE_LOADER_X(damages, damage_report_t, damages, n_damages, cap_damages, damage_report_from_json)
DEFINE_LOADER_X(tariffs, tariff_t, tariffs, n_tariffs, cap_tariffs, tariff_from_json)
DEFINE_LOADER_X(versions, tariff_version_t, versions, n_versions, cap_versions,
                tariff_version_from_json)
DEFINE_LOADER_X(user_tariffs, user_tariff_t, user_tariffs, n_user_tariffs, cap_user_tariffs,
                user_tariff_from_json)
DEFINE_LOADER_X(mandates, mandate_t, mandates, n_mandates, cap_mandates, mandate_from_json)
DEFINE_LOADER_X(runs, billing_run_t, runs, n_runs, cap_runs, billing_run_from_json)
DEFINE_LOADER_X(invoices, invoice_t, invoices, n_invoices, cap_invoices, invoice_from_json)
DEFINE_LOADER_X(lines, invoice_line_t, lines, n_lines, cap_lines, invoice_line_from_json)
DEFINE_LOADER_X(sessions, session_t, sessions, n_sessions, cap_sessions, session_from_json)
DEFINE_LOADER_X(tokens, token_t, tokens, n_tokens, cap_tokens, token_from_json)
DEFINE_LOADER_X(readings, odometer_reading_t, readings, n_readings, cap_readings,
                odometer_reading_from_json)

static int load_jobs(const cJSON *obj, void *ctx) {
  db_t *db = ctx;
  const cJSON *d = cJSON_GetObjectItemCaseSensitive(obj, "km_mail_day");
  if (!d) return 0;
  if (!cJSON_IsNumber(d) || d->valuedouble < 0 || d->valuedouble > 9007199254740991.0) return -1;
  db->km_mail_day = (int64_t)d->valuedouble;
  return 0;
}

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
  free(db->damages);
  free(db->tariffs);
  free(db->versions);
  free(db->user_tariffs);
  free(db->mandates);
  free(db->runs);
  free(db->invoices);
  free(db->lines);
  free(db->sessions);
  free(db->tokens);
  free(db->readings);
  memset(db, 0, sizeof *db);
}

/* A crash during db_finalize_run can leave invoices and lines on disk whose run was never
 * written as FINAL. Drop them (RAM only) and unlock their bookings; the next finalize
 * rewrites the files, so no invoice number is burned. */
static void drop_orphan_invoices(db_t *db) {
  size_t w = 0;
  for (size_t i = 0; i < db->n_invoices; i++) {
    const billing_run_t *r = db_run(db, db->invoices[i].run_id);
    if (r && r->state == RUN_FINAL) db->invoices[w++] = db->invoices[i];
  }
  db->n_invoices = w;
  w = 0;
  for (size_t i = 0; i < db->n_lines; i++)
    if (db_invoice(db, db->lines[i].invoice_number)) db->lines[w++] = db->lines[i];
  db->n_lines = w;
  for (size_t i = 0; i < db->n_bookings; i++)
    if (db->bookings[i].invoice_id != 0 && !db_invoice(db, db->bookings[i].invoice_id))
      db->bookings[i].invoice_id = 0;
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
  if (rc == 0) rc = load_file(db, "damage_reports.jsonl", load_damages);
  if (rc == 0) rc = load_file(db, "tariffs.jsonl", load_tariffs);
  if (rc == 0) rc = load_file(db, "tariff_versions.jsonl", load_versions);
  if (rc == 0) rc = load_file(db, "user_tariffs.jsonl", load_user_tariffs);
  if (rc == 0) rc = load_file(db, "mandates.jsonl", load_mandates);
  if (rc == 0) rc = load_file(db, "billing_runs.jsonl", load_runs);
  if (rc == 0) rc = load_file(db, "invoices.jsonl", load_invoices);
  if (rc == 0) rc = load_file(db, "invoice_lines.jsonl", load_lines);
  static const char *const subdirs[] = {"photos", "invoices", "billing"};
  for (size_t i = 0; i < sizeof subdirs / sizeof *subdirs && rc == 0; i++) {
    char d[512];
    rc = table_path(db, subdirs[i], d, sizeof d);
    if (rc == 0) rc = store_ensure_dir(d);
  }
  if (rc == 0) rc = load_file(db, "sessions.jsonl", load_sessions);
  if (rc == 0) rc = load_file(db, "tokens.jsonl", load_tokens);
  if (rc == 0) rc = load_file(db, "settings.json", load_settings);
  if (rc == 0) rc = load_file(db, "jobs.json", load_jobs);
  if (rc == 0) {
    char path[512];
    long keep = -1;
    rc = table_path(db, "odometer_readings.jsonl", path, sizeof path);
    if (rc == 0) rc = store_read_log(path, load_readings, db, &keep);
    /* Cut a torn last line, else the next append would be glued onto it. */
    if (rc == 0 && keep >= 0 && truncate(path, (off_t)keep) != 0) rc = -1;
  }
  if (rc != 0) {
    db_free(db);
    return -1;
  }
  drop_orphan_invoices(db);
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

car_t *db_car_by_imei(db_t *db, const char *imei) {
  if (!imei || !*imei) return NULL;
  for (size_t i = 0; i < db->n_cars; i++)
    if (strcmp(db->cars[i].tracker_imei, imei) == 0) return &db->cars[i];
  return NULL;
}

booking_t *db_booking(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_bookings; i++)
    if (db->bookings[i].id == id) return &db->bookings[i];
  return NULL;
}

damage_report_t *db_damage(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_damages; i++)
    if (db->damages[i].id == id) return &db->damages[i];
  return NULL;
}

token_t *db_token(db_t *db, const char *token_hash) {
  for (size_t i = 0; i < db->n_tokens; i++)
    if (strcmp(db->tokens[i].token_hash, token_hash) == 0) return &db->tokens[i];
  return NULL;
}

tariff_t *db_tariff(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_tariffs; i++)
    if (db->tariffs[i].id == id) return &db->tariffs[i];
  return NULL;
}

tariff_t *db_tariff_by_name(db_t *db, const char *name) {
  for (size_t i = 0; i < db->n_tariffs; i++)
    if (strcmp(db->tariffs[i].name, name) == 0) return &db->tariffs[i];
  return NULL;
}

tariff_version_t *db_version(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_versions; i++)
    if (db->versions[i].id == id) return &db->versions[i];
  return NULL;
}

user_tariff_t *db_user_tariff(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_user_tariffs; i++)
    if (db->user_tariffs[i].id == id) return &db->user_tariffs[i];
  return NULL;
}

mandate_t *db_mandate(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_mandates; i++)
    if (db->mandates[i].id == id) return &db->mandates[i];
  return NULL;
}

const mandate_t *db_mandate_current(const db_t *db, int64_t user_id) {
  const mandate_t *best = NULL;
  for (size_t i = 0; i < db->n_mandates; i++) {
    const mandate_t *m = &db->mandates[i];
    if (m->user_id == user_id && m->status == MANDATE_ACTIVE && (!best || m->id > best->id))
      best = m;
  }
  return best;
}

billing_run_t *db_run(db_t *db, int64_t id) {
  for (size_t i = 0; i < db->n_runs; i++)
    if (db->runs[i].id == id) return &db->runs[i];
  return NULL;
}

invoice_t *db_invoice(db_t *db, int64_t number) {
  for (size_t i = 0; i < db->n_invoices; i++)
    if (db->invoices[i].number == number) return &db->invoices[i];
  return NULL;
}

int64_t db_next_invoice_number(const db_t *db) {
  int64_t max = 0;
  for (size_t i = 0; i < db->n_invoices; i++)
    if (db->invoices[i].number > max) max = db->invoices[i].number;
  return max + 1;
}

const tariff_version_t *db_version_at(const db_t *db, int64_t tariff_id, int64_t ts) {
  const tariff_version_t *best = NULL;
  for (size_t i = 0; i < db->n_versions; i++) {
    const tariff_version_t *v = &db->versions[i];
    if (v->tariff_id == tariff_id && v->valid_from <= ts &&
        (!best || v->valid_from > best->valid_from))
      best = v;
  }
  return best;
}

const user_tariff_t *db_user_tariff_at(const db_t *db, int64_t user_id, int64_t ts) {
  const user_tariff_t *best = NULL;
  for (size_t i = 0; i < db->n_user_tariffs; i++) {
    const user_tariff_t *u = &db->user_tariffs[i];
    if (u->user_id == user_id && u->valid_from <= ts &&
        (!best || u->valid_from > best->valid_from))
      best = u;
  }
  return best;
}

const user_tariff_t *db_user_tariff_newest(const db_t *db, int64_t user_id) {
  const user_tariff_t *best = NULL;
  for (size_t i = 0; i < db->n_user_tariffs; i++) {
    const user_tariff_t *u = &db->user_tariffs[i];
    if (u->user_id == user_id && (!best || u->valid_from > best->valid_from)) best = u;
  }
  return best;
}

const tariff_version_t *db_tariff_at(const db_t *db, int64_t user_id, int64_t ts) {
  const user_tariff_t *u = db_user_tariff_at(db, user_id, ts);
  return u ? db_version_at(db, u->tariff_id, ts) : NULL;
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
DEFINE_PUT_ENTITY(db_put_damage, damage_report_t, damages, n_damages, cap_damages,
                  "damage_reports.jsonl", damage_report_to_json)
DEFINE_PUT_ENTITY(db_put_tariff, tariff_t, tariffs, n_tariffs, cap_tariffs,
                  "tariffs.jsonl", tariff_to_json)
DEFINE_PUT_ENTITY(db_put_version, tariff_version_t, versions, n_versions, cap_versions,
                  "tariff_versions.jsonl", tariff_version_to_json)
DEFINE_PUT_ENTITY(db_put_user_tariff, user_tariff_t, user_tariffs, n_user_tariffs,
                  cap_user_tariffs, "user_tariffs.jsonl", user_tariff_to_json)

DEFINE_PUT_ENTITY(db_put_mandate, mandate_t, mandates, n_mandates, cap_mandates,
                  "mandates.jsonl", mandate_to_json)

DEFINE_PUT_ENTITY(db_put_run, billing_run_t, runs, n_runs, cap_runs,
                  "billing_runs.jsonl", billing_run_to_json)

/* Removes one draft run (-1 for a FINAL run); RAM restored if persisting fails. */
int db_delete_run(db_t *db, int64_t id) {
  size_t n = db->n_runs, at = n;
  for (size_t i = 0; i < n; i++)
    if (db->runs[i].id == id) at = i;
  if (at == n || db->runs[at].state == RUN_FINAL) return -1;
  billing_run_t *copy = malloc(n * sizeof *copy);
  if (!copy) return -1;
  memcpy(copy, db->runs, n * sizeof *copy);
  memmove(db->runs + at, db->runs + at + 1, (n - at - 1) * sizeof *copy);
  db->n_runs = n - 1;
  int rc = persist_table(db, "billing_runs.jsonl", db->runs, n - 1, sizeof *copy,
                         (to_json_fn)billing_run_to_json);
  if (rc != 0) {
    memcpy(db->runs, copy, n * sizeof *copy);
    db->n_runs = n;
  }
  free(copy);
  return rc == 0 ? 0 : -1;
}

/* Snapshot of one table for restore after a failed multi-table write. */
typedef struct { void *copy; size_t n; } snap_t;

static int snap_take(snap_t *s, const void *arr, size_t n, size_t esz) {
  s->n = n;
  s->copy = n ? malloc(n * esz) : NULL;
  if (n && !s->copy) return -1;
  if (n) memcpy(s->copy, arr, n * esz);
  return 0;
}

/* Puts the snapshot back; capacity never shrinks, so the (possibly moved) array holds it. */
static void snap_restore(const snap_t *s, void *arr, size_t *n, size_t esz) {
  if (s->n) memcpy(arr, s->copy, s->n * esz);
  *n = s->n;
}

int db_put_invoices(db_t *db, const invoice_t *inv, size_t n) {
  if (n == 0) return 0;
  snap_t snap;
  if (snap_take(&snap, db->invoices, db->n_invoices, sizeof *inv) != 0) return -1;
  int rc = grow((void **)&db->invoices, &db->cap_invoices, db->n_invoices + n, sizeof *inv);
  for (size_t i = 0; i < n && rc == 0; i++) {
    invoice_t *at = db_invoice(db, inv[i].number);
    if (at)
      *at = inv[i];
    else
      db->invoices[db->n_invoices++] = inv[i];
  }
  if (rc == 0)
    rc = persist_table(db, "invoices.jsonl", db->invoices, db->n_invoices, sizeof *inv,
                       (to_json_fn)invoice_to_json);
  if (rc != 0) snap_restore(&snap, db->invoices, &db->n_invoices, sizeof *inv);
  free(snap.copy);
  return rc == 0 ? 0 : -1;
}

int db_finalize_run(db_t *db, const billing_run_t *run, const invoice_t *inv, size_t n_inv,
                    const invoice_line_t *lines, size_t n_lines) {
  if (!db_run(db, run->id) || run->state != RUN_FINAL) return -1;
  for (size_t i = 0; i < n_lines; i++) {
    const booking_t *bk = db_booking(db, lines[i].booking_id);
    if (!bk || bk->invoice_id != 0) return -1;
    bool known = false;
    for (size_t j = 0; j < n_inv; j++) known = known || inv[j].number == lines[i].invoice_number;
    if (!known) return -1;
  }
  for (size_t i = 0; i < n_inv; i++)
    for (size_t j = i + 1; j < n_inv; j++)
      if (inv[i].number == inv[j].number) return -1;
  for (size_t i = 0; i < n_inv; i++)
    if (db_invoice(db, inv[i].number)) return -1;
  snap_t s_lines, s_inv, s_book, s_runs;
  memset(&s_lines, 0, sizeof s_lines);
  memset(&s_inv, 0, sizeof s_inv);
  memset(&s_book, 0, sizeof s_book);
  memset(&s_runs, 0, sizeof s_runs);
  int rc = snap_take(&s_lines, db->lines, db->n_lines, sizeof *lines);
  if (rc == 0) rc = snap_take(&s_inv, db->invoices, db->n_invoices, sizeof *inv);
  if (rc == 0) rc = snap_take(&s_book, db->bookings, db->n_bookings, sizeof *db->bookings);
  if (rc == 0) rc = snap_take(&s_runs, db->runs, db->n_runs, sizeof *run);
  if (rc == 0)
    rc = grow((void **)&db->lines, &db->cap_lines, db->n_lines + n_lines, sizeof *lines);
  if (rc == 0)
    rc = grow((void **)&db->invoices, &db->cap_invoices, db->n_invoices + n_inv, sizeof *inv);
  bool mutated = false;
  if (rc == 0) {
    mutated = true;
    for (size_t i = 0; i < n_lines; i++) {
      db->lines[db->n_lines++] = lines[i];
      db_booking(db, lines[i].booking_id)->invoice_id = lines[i].invoice_number;
    }
    for (size_t i = 0; i < n_inv; i++) db->invoices[db->n_invoices++] = inv[i];
    *db_run(db, run->id) = *run;
    rc = persist_table(db, "invoice_lines.jsonl", db->lines, db->n_lines, sizeof *lines,
                       (to_json_fn)invoice_line_to_json);
    if (rc == 0)
      rc = persist_table(db, "invoices.jsonl", db->invoices, db->n_invoices, sizeof *inv,
                         (to_json_fn)invoice_to_json);
    if (rc == 0)
      rc = persist_table(db, "bookings.jsonl", db->bookings, db->n_bookings,
                         sizeof *db->bookings, (to_json_fn)booking_to_json);
    if (rc == 0)
      rc = persist_table(db, "billing_runs.jsonl", db->runs, db->n_runs, sizeof *run,
                         (to_json_fn)billing_run_to_json);
  }
  if (rc != 0 && mutated) {
    snap_restore(&s_lines, db->lines, &db->n_lines, sizeof *lines);
    snap_restore(&s_inv, db->invoices, &db->n_invoices, sizeof *inv);
    snap_restore(&s_book, db->bookings, &db->n_bookings, sizeof *db->bookings);
    snap_restore(&s_runs, db->runs, &db->n_runs, sizeof *run);
  }
  free(s_lines.copy);
  free(s_inv.copy);
  free(s_book.copy);
  free(s_runs.copy);
  return rc == 0 ? 0 : -1;
}

/* Removes one tariff; RAM restored if persisting fails. */
/* Removes the entity with this id (the first int64_t of every entity type) from arr and
 * persists file; RAM restored if persisting fails. -1 if unknown id or persist failed. */
static int delete_one(db_t *db, void **arr, size_t *n, size_t esz, int64_t id, const char *file,
                      to_json_fn to_json) {
  char *base = *arr;
  size_t cnt = *n, at = cnt;
  for (size_t i = 0; i < cnt; i++) {
    int64_t eid;
    memcpy(&eid, base + i * esz, sizeof eid);
    if (eid == id) at = i;
  }
  if (at == cnt) return -1;
  char *copy = malloc(cnt * esz);
  if (!copy) return -1;
  memcpy(copy, base, cnt * esz);
  memmove(base + at * esz, base + (at + 1) * esz, (cnt - at - 1) * esz);
  *n = cnt - 1;
  int rc = persist_table(db, file, base, cnt - 1, esz, to_json);
  if (rc != 0) {
    memcpy(base, copy, cnt * esz);
    *n = cnt;
  }
  free(copy);
  return rc == 0 ? 0 : -1;
}

int db_delete_tariff(db_t *db, int64_t id) {
  if (!db_tariff(db, id)) return -1;
  snap_t snap;
  if (snap_take(&snap, db->versions, db->n_versions, sizeof *db->versions) != 0) return -1;
  size_t kept = 0;
  for (size_t i = 0; i < db->n_versions; i++)
    if (db->versions[i].tariff_id != id) db->versions[kept++] = db->versions[i];
  db->n_versions = kept;
  int rc = persist_table(db, "tariff_versions.jsonl", db->versions, db->n_versions,
                         sizeof *db->versions, (to_json_fn)tariff_version_to_json);
  bool versions_written = rc == 0;
  if (rc == 0)
    rc = delete_one(db, (void **)&db->tariffs, &db->n_tariffs, sizeof *db->tariffs, id,
                    "tariffs.jsonl", (to_json_fn)tariff_to_json);
  if (rc != 0) {
    snap_restore(&snap, db->versions, &db->n_versions, sizeof *db->versions);
    /* Best effort: the def stayed on disk, so put its versions back there too. */
    if (versions_written)
      persist_table(db, "tariff_versions.jsonl", db->versions, db->n_versions,
                    sizeof *db->versions, (to_json_fn)tariff_version_to_json);
  }
  free(snap.copy);
  return rc == 0 ? 0 : -1;
}

int db_delete_version(db_t *db, int64_t id) {
  return delete_one(db, (void **)&db->versions, &db->n_versions, sizeof *db->versions, id,
                    "tariff_versions.jsonl", (to_json_fn)tariff_version_to_json);
}

int db_delete_user_tariff(db_t *db, int64_t id) {
  return delete_one(db, (void **)&db->user_tariffs, &db->n_user_tariffs,
                    sizeof *db->user_tariffs, id, "user_tariffs.jsonl",
                    (to_json_fn)user_tariff_to_json);
}

int db_photo_path(const db_t *db, int64_t report_id, int k, char *out, size_t cap) {
  int n = snprintf(out, cap, "%s/photos/r%lld-%d.jpg", db->dir, (long long)report_id, k);
  return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int db_invoice_pdf_path(const db_t *db, int64_t number, char *out, size_t cap) {
  int n = snprintf(out, cap, "%s/invoices/%lld.pdf", db->dir, (long long)number);
  return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int db_run_xml_relpath(int64_t run_id, char *out, size_t cap) {
  int n = snprintf(out, cap, "billing/run-%lld-pain008.xml", (long long)run_id);
  return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

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

int db_add_readings(db_t *db, const odometer_reading_t *r, size_t n) {
  if (n == 0) return 0;
  char path[512];
  if (table_path(db, "odometer_readings.jsonl", path, sizeof path) != 0) return -1;
  char *buf = NULL;
  size_t len = 0, cap = 0;
  int rc = 0;
  for (size_t i = 0; i < n && rc == 0; i++) {
    cJSON *j = odometer_reading_to_json(&r[i]);
    char *s = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!s || buf_append(&buf, &len, &cap, s, strlen(s)) != 0 ||
        buf_append(&buf, &len, &cap, "\n", 1) != 0)
      rc = -1;
    free(s);
  }
  if (rc == 0 && grow((void **)&db->readings, &db->cap_readings, db->n_readings + n,
                      sizeof *db->readings) != 0)
    rc = -1;
  if (rc == 0) rc = store_append(path, buf, len);
  free(buf);
  if (rc != 0) return -1;
  memcpy(db->readings + db->n_readings, r, n * sizeof *r);
  db->n_readings += n;
  return 0;
}

int db_prune_readings(db_t *db, int64_t before) {
  size_t n = db->n_readings;
  odometer_reading_t *copy = n ? malloc(n * sizeof *copy) : NULL;
  if (n && !copy) return -1;
  if (n) memcpy(copy, db->readings, n * sizeof *copy);
  size_t w = 0;
  for (size_t i = 0; i < n; i++)
    if (copy[i].ts >= before) db->readings[w++] = copy[i];
  db->n_readings = w;
  int rc = 0;
  if (w != n)
    rc = persist_table(db, "odometer_readings.jsonl", db->readings, w, sizeof *copy,
                       (to_json_fn)odometer_reading_to_json);
  if (rc != 0) {
    memcpy(db->readings, copy, n * sizeof *copy);
    db->n_readings = n;
  }
  free(copy);
  return rc == 0 ? 0 : -1;
}

int db_put_km_mail_day(db_t *db, int64_t day) {
  char path[512], line[64];
  if (table_path(db, "jobs.json", path, sizeof path) != 0) return -1;
  int k = snprintf(line, sizeof line, "{\"km_mail_day\":%lld}\n", (long long)day);
  if (k < 0 || (size_t)k >= sizeof line) return -1;
  if (store_write_atomic(path, line, (size_t)k) != 0) return -1;
  db->km_mail_day = day;
  return 0;
}
