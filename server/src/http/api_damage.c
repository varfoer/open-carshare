#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/booking_rules.h"
#include "core/codec.h"
#include "http/api_internal.h"
#include "store/store.h"

/* Booking by path id that the caller owns (or, with allow_admin, any booking for an
 * admin); NULL with res filled with 400/404 otherwise. */
static const booking_t *path_booking(app_t *a, const api_req_t *req, const api_ctx_t *ctx,
                                     bool allow_admin, api_res_t *res) {
  int64_t id;
  if (!api_segment_id(req->path, "/api/bookings/", &id)) { api_invalid(res); return NULL; }
  const booking_t *b = db_booking(&a->db, id);
  bool admin = allow_admin && ctx->user->role == ROLE_ADMIN;
  if (!b || (b->user_id != ctx->user->id && !admin)) { api_not_found(res); return NULL; }
  return b;
}

/* Report by path id, same access rule as path_booking applied to the report's booking:
 * access follows the booking's current owner, not the report's author. */
static const damage_report_t *path_report(app_t *a, const api_req_t *req, const api_ctx_t *ctx,
                                          bool allow_admin, api_res_t *res) {
  int64_t id;
  if (!api_segment_id(req->path, "/api/damage/", &id)) { api_invalid(res); return NULL; }
  const damage_report_t *d = db_damage(&a->db, id);
  const booking_t *b = d ? db_booking(&a->db, d->booking_id) : NULL;
  bool admin = allow_admin && ctx->user->role == ROLE_ADMIN;
  if (!b || (b->user_id != ctx->user->id && !admin)) { api_not_found(res); return NULL; }
  return d;
}

void h_damage_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const booking_t *bp = path_booking(a, req, ctx, false, res);
  if (!bp) return;
  booking_t b = *bp;
  damage_report_t d;
  memset(&d, 0, sizeof d);
  const char *phase = ctx->body ? api_get_str(ctx->body, "phase", 8) : NULL;
  const char *text = ctx->body ? api_get_str(ctx->body, "text", sizeof d.text) : NULL;
  if (!phase || !text || !*text) { api_invalid(res); return; }
  if (strcmp(phase, "start") == 0) {
    d.phase = DMG_START;
  } else if (strcmp(phase, "end") == 0) {
    d.phase = DMG_END;
  } else {
    api_invalid(res);
    return;
  }
  br_result_t r = br_check_damage(ctx->user, &b, d.phase);
  if (r != BR_OK) { api_br_error(res, r); return; }
  d.booking_id = b.id;
  d.user_id = ctx->user->id;
  d.created_at = req->now;
  snprintf(d.text, sizeof d.text, "%s", text);
  if (db_put_damage(&a->db, &d) != 0) { api_storage_error(res); return; }
  api_json(res, 201, damage_report_to_json(&d));
}

void h_damage_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const booking_t *b = path_booking(a, req, ctx, true, res);
  if (!b) return;
  int64_t booking_id = b->id;
  cJSON *arr = cJSON_CreateArray();
  if (!arr) { api_json(res, 500, NULL); return; }
  for (size_t i = 0; i < a->db.n_damages; i++) {
    if (a->db.damages[i].booking_id != booking_id) continue;
    cJSON *o = damage_report_to_json(&a->db.damages[i]);
    if (!o) {
      cJSON_Delete(arr);
      api_json(res, 500, NULL);
      return;
    }
    cJSON_AddItemToArray(arr, o);
  }
  api_json(res, 200, arr);
}

void h_photo_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const damage_report_t *dp = path_report(a, req, ctx, false, res);
  if (!dp) return;
  damage_report_t d = *dp;
  const unsigned char *p = (const unsigned char *)req->body;
  if (!p || req->body_len < 3 || p[0] != 0xFF || p[1] != 0xD8 || p[2] != 0xFF) {
    api_error(res, 415, "unsupported_media_type", "Bitte ein JPEG-Foto hochladen.");
    return;
  }
  if (d.n_photos >= DMG_MAX_PHOTOS) {
    api_error(res, 400, "too_many_photos", "Höchstens 10 Fotos pro Schadensmeldung.");
    return;
  }
  char path[1024];
  d.n_photos++;
  if (db_photo_path(&a->db, d.id, d.n_photos, path, sizeof path) != 0 ||
      store_write_atomic(path, req->body, req->body_len) != 0) {
    api_storage_error(res);
    return;
  }
  /* File first: a report never counts a photo that is not on disk. */
  if (db_put_damage(&a->db, &d) != 0) {
    remove(path);
    api_storage_error(res);
    return;
  }
  api_json(res, 201, damage_report_to_json(&d));
}

void h_photo_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const damage_report_t *d = path_report(a, req, ctx, true, res);
  if (!d) return;
  int64_t k;
  if (!api_path_id(strrchr(req->path, '/'), "", &k) || k < 1 || k > d->n_photos) {
    api_not_found(res);
    return;
  }
  char path[1024];
  char *buf = NULL;
  size_t len = 0;
  if (db_photo_path(&a->db, d->id, (int)k, path, sizeof path) != 0 ||
      store_read_file(path, API_PHOTO_MAX_BYTES, &buf, &len) != 0) {
    api_storage_error(res);
    return;
  }
  api_binary(res, 200, "image/jpeg", buf, len);
}
