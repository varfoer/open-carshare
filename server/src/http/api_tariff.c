#include <stdlib.h>
#include <string.h>

#include "core/booking_rules.h"
#include "core/codec.h"
#include "http/api_internal.h"

/* ---- admin tariffs and their versions (all MIN_ADMIN) ---- */

static int cmp_newest_first(const void *x, const void *y) {
  const tariff_version_t *p = *(const tariff_version_t *const *)x;
  const tariff_version_t *q = *(const tariff_version_t *const *)y;
  return p->valid_from < q->valid_from ? 1 : p->valid_from > q->valid_from ? -1 : 0;
}

cJSON *tariff_api_json(const db_t *db, const tariff_t *t) {
  cJSON *o = tariff_to_json(t);
  cJSON *arr = cJSON_CreateArray();
  const tariff_version_t **v = malloc((db->n_versions + 1) * sizeof *v);
  if (!o || !arr || !v) goto fail;
  size_t n = 0;
  for (size_t i = 0; i < db->n_versions; i++)
    if (db->versions[i].tariff_id == t->id) v[n++] = &db->versions[i];
  qsort(v, n, sizeof *v, cmp_newest_first);
  for (size_t i = 0; i < n; i++) {
    cJSON *e = tariff_version_to_json(v[i]);
    if (!e || !cJSON_AddItemToArray(arr, e)) {
      cJSON_Delete(e);
      goto fail;
    }
  }
  free(v);
  if (!cJSON_AddItemToObject(o, "versions", arr)) {
    cJSON_Delete(arr);
    cJSON_Delete(o);
    return NULL;
  }
  return o;
fail:
  free(v);
  cJSON_Delete(arr);
  cJSON_Delete(o);
  return NULL;
}

/* Raw "name" cap including padding; the trimmed name must still fit tariff_t.name. */
#define NAME_RAW_CAP 256

/* "name" from body, ASCII spaces trimmed at both ends, into out[64]. False if missing,
 * not a string, NAME_RAW_CAP+ bytes raw, or not 1..63 bytes after trimming. */
static bool read_name(const cJSON *body, char out[64]) {
  const char *s = api_get_str(body, "name", NAME_RAW_CAP);
  if (!s) return false;
  while (*s == ' ') s++;
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') n--;
  if (n == 0 || n >= 64) return false;
  memcpy(out, s, n);
  out[n] = 0;
  return true;
}

static void duplicate_name(api_res_t *res) {
  api_error(res, 409, "duplicate_name", "Diesen Tarifnamen gibt es schon.");
}

void h_admin_tariffs_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  (void)ctx;
  const db_t *db = &a->db;
  cJSON *arr = cJSON_CreateArray();
  if (!arr) { api_storage_error(res); return; }
  for (size_t i = 0; i < db->n_tariffs; i++) {
    cJSON *o = tariff_api_json(db, &db->tariffs[i]);
    if (!o || !cJSON_AddItemToArray(arr, o)) {
      cJSON_Delete(o);
      cJSON_Delete(arr);
      api_storage_error(res);
      return;
    }
  }
  api_json(res, 200, arr);
}

void h_admin_tariff_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  tariff_t t;
  memset(&t, 0, sizeof t);
  if (!read_name(ctx->body, t.name)) { api_invalid(res); return; }
  if (db_tariff_by_name(&a->db, t.name)) { duplicate_name(res); return; }
  t.created_at = req->now;
  if (db_put_tariff(&a->db, &t) != 0) { api_storage_error(res); return; }
  api_json(res, 201, tariff_api_json(&a->db, &t));
}

void h_admin_tariff_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/tariffs/", &id)) { api_invalid(res); return; }
  const tariff_t *old = db_tariff(&a->db, id);
  if (!old) { api_not_found(res); return; }
  tariff_t t = *old;
  if (!read_name(ctx->body, t.name)) { api_invalid(res); return; }
  const tariff_t *other = db_tariff_by_name(&a->db, t.name);
  if (other && other->id != id) { duplicate_name(res); return; }
  if (db_put_tariff(&a->db, &t) != 0) { api_storage_error(res); return; }
  api_json(res, 200, tariff_api_json(&a->db, &t));
}

void h_admin_tariff_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/tariffs/", &id)) { api_invalid(res); return; }
  if (!db_tariff(&a->db, id)) { api_not_found(res); return; }
  for (size_t i = 0; i < a->db.n_user_tariffs; i++) {
    if (a->db.user_tariffs[i].tariff_id == id) {
      api_error(res, 409, "tariff_in_use", "Dieser Tarif ist Nutzern zugeordnet.");
      return;
    }
  }
  if (db_delete_tariff(&a->db, id) != 0) { api_storage_error(res); return; }
  api_ok(res);
}

void h_admin_version_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t tid;
  if (!api_segment_id(req->path, "/api/admin/tariffs/", &tid)) { api_invalid(res); return; }
  if (!db_tariff(&a->db, tid)) { api_not_found(res); return; }
  if (!ctx->body) { api_invalid(res); return; }
  /* Single validator: complete the body with the server-owned fields, then parse. */
  cJSON *j = cJSON_Duplicate(ctx->body, 1);
  if (!j) { api_storage_error(res); return; }
  static const char *const own[] = {"id", "tariff_id", "created_at"};
  for (size_t i = 0; i < sizeof own / sizeof *own; i++) cJSON_DeleteItemFromObjectCaseSensitive(j, own[i]);
  tariff_version_t v;
  if (!cJSON_AddNumberToObject(j, "id", 0) || !cJSON_AddNumberToObject(j, "tariff_id", (double)tid) ||
      !cJSON_AddNumberToObject(j, "created_at", (double)req->now)) {
    cJSON_Delete(j);
    api_storage_error(res);
    return;
  }
  int rc = tariff_version_from_json(j, &v);
  cJSON_Delete(j);
  if (rc != 0 || !br_time_in_range(v.valid_from, req->now)) { api_invalid(res); return; }
  bool has_version = false;
  for (size_t i = 0; i < a->db.n_versions; i++) {
    if (a->db.versions[i].tariff_id != tid) continue;
    has_version = true;
    if (a->db.versions[i].valid_from == v.valid_from) {
      api_error(res, 409, "duplicate_valid_from", "Für diesen Zeitpunkt gibt es schon einen Tarif.");
      return;
    }
  }
  if (has_version && v.valid_from < req->now) {
    api_error(res, 409, "tariff_in_past", "Ein neuer Tarif darf erst ab heute gelten.");
    return;
  }
  if (db_put_version(&a->db, &v) != 0) { api_storage_error(res); return; }
  api_json(res, 201, tariff_version_to_json(&v));
}

void h_admin_version_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/tariff-versions/", &id)) { api_invalid(res); return; }
  const tariff_version_t *v = db_version(&a->db, id);
  if (!v) { api_not_found(res); return; }
  if (v->valid_from <= req->now) {
    api_error(res, 409, "tariff_active", "Ein Tarif, der schon gilt, kann nicht gelöscht werden.");
    return;
  }
  if (db_delete_version(&a->db, id) != 0) { api_storage_error(res); return; }
  api_ok(res);
}

/* ---- user tariff assignments (all MIN_ADMIN) ---- */

static int cmp_assign_newest_first(const void *x, const void *y) {
  const user_tariff_t *p = *(const user_tariff_t *const *)x;
  const user_tariff_t *q = *(const user_tariff_t *const *)y;
  return p->valid_from < q->valid_from ? 1 : p->valid_from > q->valid_from ? -1 : 0;
}

static void duplicate_assignment(api_res_t *res) {
  api_error(res, 409, "duplicate_valid_from", "Für diesen Zeitpunkt gibt es schon eine Zuordnung.");
}

/* "tariff_id" of an existing tariff into *out. */
static bool read_tariff_id(db_t *db, const cJSON *body, int64_t *out) {
  return api_get_int(body, "tariff_id", out) && db_tariff(db, *out);
}

/* "valid_from" within br_time_in_range into *out. */
static bool read_valid_from(const cJSON *body, int64_t now, int64_t *out) {
  return api_get_int(body, "valid_from", out) && br_time_in_range(*out, now);
}

/* Any invoiced booking of the user with booked_start >= from. */
static bool billed_since(const db_t *db, int64_t user_id, int64_t from) {
  for (size_t i = 0; i < db->n_bookings; i++) {
    const booking_t *b = &db->bookings[i];
    if (b->user_id == user_id && b->invoice_id != 0 && b->booked_start >= from) return true;
  }
  return false;
}

/* 404, then 409 assignment_not_newest. The assignment, or NULL with res filled. */
static const user_tariff_t *newest_from_path(app_t *a, const api_req_t *req, api_res_t *res) {
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/user-tariffs/", &id)) { api_invalid(res); return NULL; }
  const user_tariff_t *u = db_user_tariff(&a->db, id);
  if (!u) { api_not_found(res); return NULL; }
  const user_tariff_t *n = db_user_tariff_newest(&a->db, u->user_id);
  if (n != u) {
    api_error(res, 409, "assignment_not_newest", "Nur die neueste Tarifzuordnung kann geändert werden.");
    return NULL;
  }
  return u;
}

static void billed(api_res_t *res) {
  api_error(res, 409, "assignment_billed", "Es gibt schon abgerechnete Fahrten in diesem Zeitraum.");
}

void h_admin_user_tariffs_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  int64_t uid;
  if (!api_segment_id(req->path, "/api/admin/users/", &uid)) { api_invalid(res); return; }
  if (!db_user(&a->db, uid)) { api_not_found(res); return; }
  const db_t *db = &a->db;
  cJSON *arr = cJSON_CreateArray();
  const user_tariff_t **v = malloc((db->n_user_tariffs + 1) * sizeof *v);
  if (!arr || !v) goto fail;
  size_t n = 0;
  for (size_t i = 0; i < db->n_user_tariffs; i++)
    if (db->user_tariffs[i].user_id == uid) v[n++] = &db->user_tariffs[i];
  qsort(v, n, sizeof *v, cmp_assign_newest_first);
  for (size_t i = 0; i < n; i++) {
    cJSON *e = user_tariff_to_json(v[i]);
    if (!e || !cJSON_AddItemToArray(arr, e)) {
      cJSON_Delete(e);
      goto fail;
    }
  }
  free(v);
  api_json(res, 200, arr);
  return;
fail:
  free(v);
  cJSON_Delete(arr);
  api_storage_error(res);
}

void h_admin_user_tariff_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t uid;
  if (!api_segment_id(req->path, "/api/admin/users/", &uid)) { api_invalid(res); return; }
  if (!db_user(&a->db, uid)) { api_not_found(res); return; }
  user_tariff_t u;
  memset(&u, 0, sizeof u);
  if (!read_tariff_id(&a->db, ctx->body, &u.tariff_id) ||
      !read_valid_from(ctx->body, req->now, &u.valid_from)) {
    api_invalid(res);
    return;
  }
  for (size_t i = 0; i < a->db.n_user_tariffs; i++) {
    const user_tariff_t *o = &a->db.user_tariffs[i];
    if (o->user_id == uid && o->valid_from == u.valid_from) { duplicate_assignment(res); return; }
  }
  if (u.valid_from < req->now) {
    api_error(res, 409, "assignment_in_past", "Ein Tarifwechsel darf erst ab heute gelten.");
    return;
  }
  u.user_id = uid;
  u.created_at = req->now;
  u.created_by = ctx->user->id;
  if (db_put_user_tariff(&a->db, &u) != 0) { api_storage_error(res); return; }
  api_json(res, 201, user_tariff_to_json(&u));
}

void h_admin_user_tariff_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const user_tariff_t *old = newest_from_path(a, req, res);
  if (!old) return;
  user_tariff_t u = *old;
  bool has_tid = cJSON_GetObjectItemCaseSensitive(ctx->body, "tariff_id") != NULL;
  bool has_vf = cJSON_GetObjectItemCaseSensitive(ctx->body, "valid_from") != NULL;
  if ((!has_tid && !has_vf) || (has_tid && !read_tariff_id(&a->db, ctx->body, &u.tariff_id)) ||
      (has_vf && !read_valid_from(ctx->body, req->now, &u.valid_from))) {
    api_invalid(res);
    return;
  }
  /* The user's assignment just before this one; u.valid_from must stay after it. */
  const user_tariff_t *prev = NULL;
  for (size_t i = 0; i < a->db.n_user_tariffs; i++) {
    const user_tariff_t *o = &a->db.user_tariffs[i];
    if (o->user_id == old->user_id && o->valid_from < old->valid_from &&
        (!prev || o->valid_from > prev->valid_from))
      prev = o;
  }
  if (prev && u.valid_from == prev->valid_from) { duplicate_assignment(res); return; }
  if (prev && u.valid_from < prev->valid_from) { api_invalid(res); return; }
  int64_t from = u.valid_from < old->valid_from ? u.valid_from : old->valid_from;
  if (billed_since(&a->db, old->user_id, from)) { billed(res); return; }
  if (db_put_user_tariff(&a->db, &u) != 0) { api_storage_error(res); return; }
  api_json(res, 200, user_tariff_to_json(&u));
}

void h_admin_user_tariff_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  const user_tariff_t *old = newest_from_path(a, req, res);
  if (!old) return;
  if (billed_since(&a->db, old->user_id, old->valid_from)) { billed(res); return; }
  if (db_delete_user_tariff(&a->db, old->id) != 0) { api_storage_error(res); return; }
  api_ok(res);
}
