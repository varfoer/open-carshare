#include <stdlib.h>
#include <string.h>

#include "core/booking_rules.h"
#include "core/codec.h"
#include "http/api_internal.h"

#define CAL_MAX_RANGE (62 * 86400)

void h_cars_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  (void)ctx;
  cJSON *arr = cJSON_CreateArray();
  if (!arr) { api_storage_error(res); return; }
  for (size_t i = 0; i < a->db.n_cars; i++) {
    const car_t *c = &a->db.cars[i];
    if (!c->active) continue;
    cJSON *o = cJSON_CreateObject();
    if (!o || !cJSON_AddNumberToObject(o, "id", (double)c->id) ||
        !cJSON_AddStringToObject(o, "name", c->name) ||
        !cJSON_AddStringToObject(o, "plate", c->plate) ||
        !cJSON_AddStringToObject(o, "location", c->location)) {
      cJSON_Delete(o);
      cJSON_Delete(arr);
      api_json(res, 500, NULL);
      return;
    }
    cJSON_AddItemToArray(arr, o);
  }
  api_json(res, 200, arr);
}

typedef enum { SCOPE_CALENDAR, SCOPE_UPCOMING, SCOPE_HISTORY } scope_t;

static const db_t *sort_db;
static int cmp_idx(const void *x, const void *y) {
  const booking_t *p = &sort_db->bookings[*(const size_t *)x];
  const booking_t *q = &sort_db->bookings[*(const size_t *)y];
  if (p->booked_start != q->booked_start) return p->booked_start < q->booked_start ? -1 : 1;
  return p->id < q->id ? -1 : p->id > q->id;
}
static int cmp_idx_desc(const void *x, const void *y) { return cmp_idx(y, x); }

/* Indices of bookings passing the scope filter, sorted by start (history: newest first). */
static size_t *collect(const db_t *db, int64_t car_id, int64_t user_id, int64_t w0, int64_t w1,
                       int64_t now, scope_t scope, size_t *n_out) {
  size_t *idx = malloc((db->n_bookings + 1) * sizeof *idx);
  if (!idx) return NULL;
  size_t n = 0;
  for (size_t i = 0; i < db->n_bookings; i++) {
    const booking_t *b = &db->bookings[i];
    bool live = b->state == BK_RESERVED || b->state == BK_ACTIVE;
    if (scope == SCOPE_HISTORY) {
      if (live || b->user_id != user_id) continue;
      if (b->state != BK_DONE && b->state != BK_CANCELLED) continue;
    } else if (!live) {
      continue;
    } else if (scope == SCOPE_CALENDAR) {
      if (b->car_id != car_id) continue;
      if (!interval_overlaps(booking_occupied_start(b), booking_occupied_end(b, now), w0, w1)) continue;
    } else {
      if (b->user_id != user_id) continue;
      if (b->state != BK_ACTIVE && b->booked_end <= now) continue;
    }
    idx[n++] = i;
  }
  sort_db = db;
  qsort(idx, n, sizeof *idx, scope == SCOPE_HISTORY ? cmp_idx_desc : cmp_idx);
  *n_out = n;
  return idx;
}

void h_calendar_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t car, from, to;
  if (!api_query_int(req->query, "car", &car) || !api_query_int(req->query, "from", &from) ||
      !api_query_int(req->query, "to", &to) || to <= from) {
    api_invalid(res);
    return;
  }
  if (to - from > CAL_MAX_RANGE) {
    api_error(res, 400, "range_too_large", "Der Zeitraum ist zu groß.");
    return;
  }
  int64_t w0 = from > req->now ? from : req->now;
  cJSON *arr = cJSON_CreateArray();
  if (!arr) { api_storage_error(res); return; }
  size_t n = 0;
  size_t *idx = NULL;
  if (w0 < to) {
    idx = collect(&a->db, car, 0, w0, to, req->now, SCOPE_CALENDAR, &n);
    if (!idx) { cJSON_Delete(arr); api_storage_error(res); return; }
  }
  for (size_t k = 0; k < n; k++) {
    const booking_t *b = &a->db.bookings[idx[k]];
    bool own = b->user_id == ctx->user->id;
    cJSON *o = cJSON_CreateObject();
    bool ok = o && cJSON_AddNumberToObject(o, "start", (double)booking_occupied_start(b)) &&
              cJSON_AddNumberToObject(o, "end", (double)booking_occupied_end(b, req->now));
    if (ok && own)
      ok = cJSON_AddNumberToObject(o, "id", (double)b->id) &&
           cJSON_AddBoolToObject(o, "own", 1) &&
           cJSON_AddBoolToObject(o, "club_trip", b->club_trip) &&
           cJSON_AddStringToObject(o, "state", b->state == BK_ACTIVE ? "active" : "reserved");
    else if (ok)
      ok = cJSON_AddBoolToObject(o, "own", 0) != NULL;
    if (!ok) {
      cJSON_Delete(o);
      cJSON_Delete(arr);
      free(idx);
      api_json(res, 500, NULL);
      return;
    }
    cJSON_AddItemToArray(arr, o);
  }
  free(idx);
  api_json(res, 200, arr);
}

/* Value of a "scope=" pair in the query as upcoming|history; false if malformed. */
static bool query_scope(const char *query, scope_t *out) {
  for (const char *p = query; p;) {
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= 6 && strncmp(p, "scope=", 6) == 0) {
      if (len - 6 == 8 && strncmp(p + 6, "upcoming", 8) == 0) { *out = SCOPE_UPCOMING; return true; }
      if (len - 6 == 7 && strncmp(p + 6, "history", 7) == 0) { *out = SCOPE_HISTORY; return true; }
      return false;
    }
    p = end ? end + 1 : NULL;
  }
  return true;
}

void h_bookings_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  scope_t scope = SCOPE_UPCOMING;
  if (req->query && !query_scope(req->query, &scope)) {
    api_invalid(res);
    return;
  }
  size_t n = 0;
  size_t *idx = collect(&a->db, 0, ctx->user->id, 0, 0, req->now, scope, &n);
  cJSON *arr = cJSON_CreateArray();
  if (!idx || !arr) {
    free(idx);
    cJSON_Delete(arr);
    api_storage_error(res);
    return;
  }
  for (size_t k = 0; k < n; k++) {
    cJSON *o = api_booking_json(a, &a->db.bookings[idx[k]]);
    if (!o) {
      free(idx);
      cJSON_Delete(arr);
      api_json(res, 500, NULL);
      return;
    }
    cJSON_AddItemToArray(arr, o);
  }
  free(idx);
  api_json(res, 200, arr);
}

/* The permission also binds admins on the user endpoints (their override is the admin API). */
static bool admin_lacks_club(const api_ctx_t *ctx, bool wanted, bool before) {
  return ctx->user->role == ROLE_ADMIN && wanted && !before && !ctx->user->may_club_trip;
}

void h_booking_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t car, start, end;
  if (!ctx->body || !api_get_int(ctx->body, "car_id", &car) ||
      !api_get_int(ctx->body, "start", &start) || !api_get_int(ctx->body, "end", &end)) {
    api_invalid(res);
    return;
  }
  booking_t b;
  memset(&b, 0, sizeof b);
  b.car_id = car;
  b.user_id = ctx->user->id;
  b.booked_start = start;
  b.booked_end = end;
  b.state = BK_RESERVED;
  if (ctx->body && cJSON_GetObjectItemCaseSensitive(ctx->body, "club_trip") &&
      !api_get_bool(ctx->body, "club_trip", &b.club_trip)) {
    api_invalid(res);
    return;
  }
  if (admin_lacks_club(ctx, b.club_trip, false)) { api_br_error(res, BR_CLUB_FORBIDDEN); return; }
  br_result_t r = br_check_create(&a->db, ctx->user, &b, req->now);
  if (r != BR_OK) { api_br_error(res, r); return; }
  b.created_at = b.modified_at = req->now;
  b.modified_by = ctx->user->id;
  if (db_put_booking(&a->db, &b) != 0) { api_storage_error(res); return; }
  api_json(res, 201, api_booking_json(a, &b));
}

/* Own booking by path id, or NULL (res already filled with 400/404). */
static const booking_t *own_booking(app_t *a, const api_req_t *req, const api_ctx_t *ctx,
                                    api_res_t *res) {
  int64_t id;
  if (!api_segment_id(req->path, "/api/bookings/", &id)) { api_invalid(res); return NULL; }
  const booking_t *b = db_booking(&a->db, id);
  if (!b || b->user_id != ctx->user->id) { api_not_found(res); return NULL; }
  return b;
}

void h_booking_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const booking_t *old_p = own_booking(a, req, ctx, res);
  if (!old_p) return;
  booking_t old = *old_p, nb = old;
  if (!ctx->body || !api_get_int(ctx->body, "start", &nb.booked_start) ||
      !api_get_int(ctx->body, "end", &nb.booked_end)) {
    api_invalid(res);
    return;
  }
  if (cJSON_GetObjectItemCaseSensitive(ctx->body, "club_trip") &&
      !api_get_bool(ctx->body, "club_trip", &nb.club_trip)) {
    api_invalid(res);
    return;
  }
  if (admin_lacks_club(ctx, nb.club_trip, old.club_trip)) { api_br_error(res, BR_CLUB_FORBIDDEN); return; }
  br_result_t r = br_check_update(&a->db, ctx->user, &old, &nb, req->now);
  if (r != BR_OK) { api_br_error(res, r); return; }
  nb.modified_at = req->now;
  nb.modified_by = ctx->user->id;
  if (db_put_booking(&a->db, &nb) != 0) { api_storage_error(res); return; }
  api_json(res, 200, api_booking_json(a, &nb));
}

void h_booking_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const booking_t *old_p = own_booking(a, req, ctx, res);
  if (!old_p) return;
  booking_t b = *old_p;
  br_result_t r = br_check_cancel(ctx->user, &b, req->now);
  if (r != BR_OK) { api_br_error(res, r); return; }
  b.cancel_fee_cents = br_cancel_fee(&a->db.settings, &b, req->now);
  b.state = BK_CANCELLED;
  b.cancelled_at = req->now;
  b.modified_at = req->now;
  b.modified_by = ctx->user->id;
  if (db_put_booking(&a->db, &b) != 0) { api_storage_error(res); return; }
  api_json(res, 200, api_booking_json(a, &b));
}

void h_booking_start(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const booking_t *old_p = own_booking(a, req, ctx, res);
  if (!old_p) return;
  booking_t b = *old_p;
  br_result_t r = br_check_start(&a->db, ctx->user, &b, req->now);
  if (r != BR_OK) { api_br_error(res, r); return; }
  b.state = BK_ACTIVE;
  b.actual_start = req->now;
  b.modified_at = req->now;
  b.modified_by = ctx->user->id;
  if (db_put_booking(&a->db, &b) != 0) { api_storage_error(res); return; }
  api_json(res, 200, api_booking_json(a, &b));
}

void h_booking_return(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const booking_t *old_p = own_booking(a, req, ctx, res);
  if (!old_p) return;
  booking_t b = *old_p;
  br_result_t r = br_check_return(ctx->user, &b);
  if (r != BR_OK) { api_br_error(res, r); return; }
  b.state = BK_DONE;
  b.actual_end = req->now;
  b.km_pending = b.km_source == KM_NONE;
  b.modified_at = req->now;
  b.modified_by = ctx->user->id;
  if (db_put_booking(&a->db, &b) != 0) { api_storage_error(res); return; }
  api_json(res, 200, api_booking_json(a, &b));
}

int api_expire_bookings(app_t *a, int64_t now) {
  int n = 0;
  /* db_put_booking may invalidate pointers into the table: index, copy, put. */
  for (size_t i = 0; i < a->db.n_bookings; i++) {
    const booking_t *p = &a->db.bookings[i];
    if (p->state != BK_RESERVED || p->booked_end > now) continue;
    booking_t b = *p;
    b.state = BK_DONE;
    b.km_pending = false;
    b.modified_at = now;
    b.modified_by = 0;
    if (db_put_booking(&a->db, &b) != 0) return -1;
    n++;
  }
  return n;
}

void h_settings_public(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  (void)ctx;
  cJSON *o = cJSON_CreateObject();
  if (!o || !cJSON_AddNumberToObject(o, "start_tolerance_min", a->db.settings.start_tolerance_min) ||
      !cJSON_AddNumberToObject(o, "cancel_deadline_min", a->db.settings.cancel_deadline_min) ||
      !cJSON_AddNumberToObject(o, "cancel_fee_cents", (double)a->db.settings.cancel_fee_cents)) {
    cJSON_Delete(o);
    api_json(res, 500, NULL);
    return;
  }
  api_json(res, 200, o);
}
