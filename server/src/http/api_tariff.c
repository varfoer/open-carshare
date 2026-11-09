#include <stdlib.h>
#include <string.h>

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

/* "name" from body, ASCII spaces trimmed at both ends, into out[64]. False if missing,
 * not a string, 64+ bytes or empty after trimming. */
static bool read_name(const cJSON *body, char out[64]) {
  const char *s = api_get_str(body, "name", 64);
  if (!s) return false;
  while (*s == ' ') s++;
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') n--;
  if (n == 0) return false;
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
  if (rc != 0) { api_invalid(res); return; }
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
