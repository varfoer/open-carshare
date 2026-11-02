#include <stdlib.h>
#include <string.h>

#include "core/codec.h"
#include "http/api_internal.h"

/* ---- admin tariffs (all MIN_ADMIN) ---- */

static int cmp_newest_first(const void *x, const void *y) {
  const tariff_t *p = *(const tariff_t *const *)x, *q = *(const tariff_t *const *)y;
  return p->valid_from < q->valid_from ? 1 : p->valid_from > q->valid_from ? -1 : 0;
}

void h_admin_tariffs_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  int64_t car;
  if (!api_segment_id(req->path, "/api/admin/cars/", &car)) { api_invalid(res); return; }
  const db_t *db = &a->db;
  if (!db_car((db_t *)db, car)) { api_not_found(res); return; }
  const tariff_t **v = malloc((db->n_tariffs + 1) * sizeof *v);
  cJSON *arr = cJSON_CreateArray();
  if (!v || !arr) { free(v); cJSON_Delete(arr); api_storage_error(res); return; }
  size_t n = 0;
  for (size_t i = 0; i < db->n_tariffs; i++)
    if (db->tariffs[i].car_id == car) v[n++] = &db->tariffs[i];
  qsort(v, n, sizeof *v, cmp_newest_first);
  for (size_t i = 0; i < n; i++) {
    cJSON *o = tariff_to_json(v[i]);
    if (!o || !cJSON_AddItemToArray(arr, o)) {
      cJSON_Delete(o);
      cJSON_Delete(arr);
      free(v);
      api_storage_error(res);
      return;
    }
  }
  free(v);
  api_json(res, 200, arr);
}

void h_admin_tariff_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t car;
  if (!api_segment_id(req->path, "/api/admin/cars/", &car)) { api_invalid(res); return; }
  if (!db_car(&a->db, car)) { api_not_found(res); return; }
  if (!ctx->body) { api_invalid(res); return; }
  /* Single validator: complete the body with the server-owned fields, then parse. */
  cJSON *j = cJSON_Duplicate(ctx->body, 1);
  if (!j) { api_storage_error(res); return; }
  static const char *const own[] = {"id", "car_id", "created_at"};
  for (size_t i = 0; i < sizeof own / sizeof *own; i++) cJSON_DeleteItemFromObjectCaseSensitive(j, own[i]);
  tariff_t t;
  if (!cJSON_AddNumberToObject(j, "id", 0) || !cJSON_AddNumberToObject(j, "car_id", (double)car) ||
      !cJSON_AddNumberToObject(j, "created_at", (double)req->now)) {
    cJSON_Delete(j);
    api_storage_error(res);
    return;
  }
  int rc = tariff_from_json(j, &t);
  cJSON_Delete(j);
  if (rc != 0) { api_invalid(res); return; }
  bool has_version = false;
  for (size_t i = 0; i < a->db.n_tariffs; i++) {
    if (a->db.tariffs[i].car_id != car) continue;
    has_version = true;
    if (a->db.tariffs[i].valid_from == t.valid_from) {
      api_error(res, 409, "duplicate_valid_from", "Für diesen Zeitpunkt gibt es schon einen Tarif.");
      return;
    }
  }
  if (has_version && t.valid_from < req->now) {
    api_error(res, 409, "tariff_in_past", "Ein neuer Tarif darf erst ab heute gelten.");
    return;
  }
  if (db_put_tariff(&a->db, &t) != 0) { api_storage_error(res); return; }
  api_json(res, 201, tariff_to_json(&t));
}

void h_admin_tariff_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/tariffs/", &id)) { api_invalid(res); return; }
  const tariff_t *t = db_tariff(&a->db, id);
  if (!t) { api_not_found(res); return; }
  if (t->valid_from <= req->now) {
    api_error(res, 409, "tariff_active", "Ein Tarif, der schon gilt, kann nicht gelöscht werden.");
    return;
  }
  if (db_delete_tariff(&a->db, id) != 0) { api_storage_error(res); return; }
  api_ok(res);
}
