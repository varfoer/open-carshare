#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/session.h"
#include "core/booking_rules.h"
#include "core/codec.h"
#include "http/api_internal.h"

#define FEE_MAX ((int64_t)1 << 53)

/* ---- helpers ---- */

static void err_invalid_email(api_res_t *res) {
  api_error(res, 400, "invalid_email", "Ungültige E-Mail-Adresse.");
}
static void err_duplicate_email(api_res_t *res) {
  api_error(res, 409, "duplicate_email", "Diese E-Mail-Adresse ist bereits registriert.");
}

/* Trim and lowercase (ASCII) into out. 0 ok, -1 no exactly-one-'@' address or too long. */
static int normalize_email(const char *in, char out[128]) {
  while (*in && isspace((unsigned char)*in)) in++;
  size_t n = strlen(in);
  while (n > 0 && isspace((unsigned char)in[n - 1])) n--;
  if (n == 0 || n >= 128) return -1;
  size_t ats = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '@') ats++;
    if (isspace(c)) return -1;
    out[i] = (char)tolower(c);
  }
  out[n] = 0;
  if (ats != 1 || out[0] == '@' || out[n - 1] == '@') return -1;
  return 0;
}

/* Id of the path segment right after prefix (ends at '/' or end of path). */
static bool segment_id(const char *path, const char *prefix, int64_t *id) {
  size_t pl = strlen(prefix);
  if (strncmp(path, prefix, pl) != 0) return false;
  const char *rest = path + pl;
  const char *slash = strchr(rest, '/');
  size_t n = slash ? (size_t)(slash - rest) : strlen(rest);
  char tmp[48];
  if (n == 0 || n > 20) return false;
  tmp[0] = '/';
  memcpy(tmp + 1, rest, n);
  tmp[n + 1] = 0;
  return api_path_id(tmp, "/", id);
}

/* Query key present at all (so a malformed value is 400, not "absent"). */
static bool query_has(const char *q, const char *key) {
  size_t kl = strlen(key);
  for (const char *p = q; p && *p;) {
    if (strncmp(p, key, kl) == 0 && p[kl] == '=') return true;
    p = strchr(p, '&');
    if (p) p++;
  }
  return false;
}

/* Optional int query param. 0 absent, 1 present, -1 malformed. */
static int query_opt(const char *q, const char *key, int64_t *out) {
  if (!query_has(q, key)) return 0;
  return api_query_int(q, key, out) ? 1 : -1;
}

/* ---- users ---- */

void h_admin_users_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  (void)ctx;
  cJSON *arr = cJSON_CreateArray();
  if (!arr) { api_storage_error(res); return; }
  for (size_t i = 0; i < a->db.n_users; i++) {
    cJSON *o = user_to_public_json(&a->db.users[i]);
    if (!o) { cJSON_Delete(arr); api_json(res, 500, NULL); return; }
    cJSON_AddItemToArray(arr, o);
  }
  api_json(res, 200, arr);
}

/* Reads the editable user fields from body into u (email normalized).
 * Returns false with res filled on error. */
static bool read_user_fields(const cJSON *body, user_t *u, api_res_t *res) {
  const char *email = body ? api_get_str(body, "email", 1024) : NULL;
  const char *name = body ? api_get_str(body, "name", sizeof u->name) : NULL;
  const char *street = body ? api_get_str(body, "street", sizeof u->street) : NULL;
  const char *zip = body ? api_get_str(body, "zip", sizeof u->zip) : NULL;
  const char *city = body ? api_get_str(body, "city", sizeof u->city) : NULL;
  const char *role = body ? api_get_str(body, "role", 16) : NULL;
  if (!email || !name || !street || !zip || !city || !role || !*name) { api_invalid(res); return false; }
  if (strcmp(role, "user") == 0) u->role = ROLE_USER;
  else if (strcmp(role, "admin") == 0) u->role = ROLE_ADMIN;
  else { api_invalid(res); return false; }
  const cJSON *lc = cJSON_GetObjectItemCaseSensitive(body, "license_checked_at");
  if (lc) {
    int64_t v;
    if (!api_get_int(body, "license_checked_at", &v) || v < 0) { api_invalid(res); return false; }
    u->license_checked_at = v;
  }
  if (normalize_email(email, u->email) != 0) { err_invalid_email(res); return false; }
  snprintf(u->name, sizeof u->name, "%s", name);
  snprintf(u->street, sizeof u->street, "%s", street);
  snprintf(u->zip, sizeof u->zip, "%s", zip);
  snprintf(u->city, sizeof u->city, "%s", city);
  return true;
}

void h_admin_user_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  user_t u;
  memset(&u, 0, sizeof u);
  if (!read_user_fields(ctx->body, &u, res)) return;
  if (db_user_by_email(&a->db, u.email)) { err_duplicate_email(res); return; }
  u.active = true;
  u.created_at = req->now;
  if (db_put_user(&a->db, &u) != 0) { api_storage_error(res); return; }
  int rc = api_send_token_mail(a, u.id, TOKEN_INVITE, req->now);
  if (rc == -1 || rc == -2) { api_storage_error(res); return; }
  cJSON *j = cJSON_CreateObject();
  cJSON *pu = user_to_public_json(&u);
  if (!j || !pu || !cJSON_AddItemToObject(j, "user", pu)) {
    cJSON_Delete(pu);
    cJSON_Delete(j);
    api_json(res, 500, NULL);
    return;
  }
  if (!cJSON_AddBoolToObject(j, "mail_sent", rc == 0)) {
    cJSON_Delete(j);
    api_json(res, 500, NULL);
    return;
  }
  api_json(res, 201, j);
}

void h_admin_user_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/users/", &id)) { api_invalid(res); return; }
  const user_t *old_p = db_user(&a->db, id);
  if (!old_p) { api_not_found(res); return; }
  user_t u = *old_p;
  bool active;
  if (!read_user_fields(ctx->body, &u, res)) return;
  if (!api_get_bool(ctx->body, "active", &active)) { api_invalid(res); return; }
  const user_t *other = db_user_by_email(&a->db, u.email);
  if (other && other->id != id) { err_duplicate_email(res); return; }
  if (id == ctx->user->id && (!active || u.role != ROLE_ADMIN)) {
    api_error(res, 400, "self", "Du kannst dich nicht selbst deaktivieren oder herabstufen.");
    return;
  }
  bool deactivated = old_p->active && !active;
  u.active = active;
  if (db_put_user(&a->db, &u) != 0) { api_storage_error(res); return; }
  if (deactivated && db_delete_sessions(&a->db, NULL, id) != 0) { api_storage_error(res); return; }
  api_user_response(res, 200, &u);
}

void h_admin_user_invite(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  int64_t id;
  if (!segment_id(req->path, "/api/admin/users/", &id)) { api_invalid(res); return; }
  int rc = api_send_token_mail(a, id, TOKEN_INVITE, req->now);
  if (rc == -2) api_not_found(res);
  else if (rc == -1) api_storage_error(res);
  else if (rc == -3) api_error(res, 500, "mail", "Die E-Mail konnte nicht gesendet werden.");
  else api_ok(res);
}

/* ---- cars ---- */

void h_admin_cars_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  (void)ctx;
  cJSON *arr = cJSON_CreateArray();
  if (!arr) { api_storage_error(res); return; }
  for (size_t i = 0; i < a->db.n_cars; i++) {
    cJSON *o = car_to_json(&a->db.cars[i]);
    if (!o) { cJSON_Delete(arr); api_json(res, 500, NULL); return; }
    cJSON_AddItemToArray(arr, o);
  }
  api_json(res, 200, arr);
}

static bool read_car_fields(const cJSON *body, car_t *c, api_res_t *res) {
  const char *name = body ? api_get_str(body, "name", sizeof c->name) : NULL;
  const char *plate = body ? api_get_str(body, "plate", sizeof c->plate) : NULL;
  const char *loc = body ? api_get_str(body, "location", sizeof c->location) : NULL;
  bool active;
  if (!name || !plate || !loc || !*name || !*plate || !api_get_bool(body, "active", &active)) {
    api_invalid(res);
    return false;
  }
  snprintf(c->name, sizeof c->name, "%s", name);
  snprintf(c->plate, sizeof c->plate, "%s", plate);
  snprintf(c->location, sizeof c->location, "%s", loc);
  c->active = active;
  return true;
}

void h_admin_car_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  car_t c;
  memset(&c, 0, sizeof c);
  if (!read_car_fields(ctx->body, &c, res)) return;
  if (db_put_car(&a->db, &c) != 0) { api_storage_error(res); return; }
  api_json(res, 201, car_to_json(&c));
}

void h_admin_car_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/cars/", &id)) { api_invalid(res); return; }
  const car_t *old = db_car(&a->db, id);
  if (!old) { api_not_found(res); return; }
  car_t c = *old;
  if (!read_car_fields(ctx->body, &c, res)) return;
  if (db_put_car(&a->db, &c) != 0) { api_storage_error(res); return; }
  api_json(res, 200, car_to_json(&c));
}

/* ---- bookings ---- */

static const db_t *sort_db;
static int cmp_idx(const void *x, const void *y) {
  const booking_t *p = &sort_db->bookings[*(const size_t *)x];
  const booking_t *q = &sort_db->bookings[*(const size_t *)y];
  if (p->booked_start != q->booked_start) return p->booked_start < q->booked_start ? -1 : 1;
  return p->id < q->id ? -1 : p->id > q->id;
}

/* Booking json + user_name/user_email. NULL on OOM. */
static cJSON *booking_admin_json(const db_t *db, const booking_t *b) {
  cJSON *o = booking_to_json(b);
  if (!o) return NULL;
  const user_t *u = NULL;
  for (size_t i = 0; i < db->n_users; i++)
    if (db->users[i].id == b->user_id) u = &db->users[i];
  if (!cJSON_AddStringToObject(o, "user_name", u ? u->name : "") ||
      !cJSON_AddStringToObject(o, "user_email", u ? u->email : "")) {
    cJSON_Delete(o);
    return NULL;
  }
  return o;
}

void h_admin_bookings_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  int64_t from = 0, to = 0, car = 0;
  int hf = query_opt(req->query, "from", &from), ht = query_opt(req->query, "to", &to);
  int hc = query_opt(req->query, "car", &car);
  if (hf < 0 || ht < 0 || hc < 0) { api_invalid(res); return; }
  const db_t *db = &a->db;
  size_t *idx = malloc((db->n_bookings + 1) * sizeof *idx);
  cJSON *arr = cJSON_CreateArray();
  if (!idx || !arr) { free(idx); cJSON_Delete(arr); api_storage_error(res); return; }
  size_t n = 0;
  for (size_t i = 0; i < db->n_bookings; i++) {
    const booking_t *b = &db->bookings[i];
    if (hc && b->car_id != car) continue;
    if (hf && ht && !interval_overlaps(b->booked_start, b->booked_end, from, to)) continue;
    idx[n++] = i;
  }
  sort_db = db;
  qsort(idx, n, sizeof *idx, cmp_idx);
  for (size_t k = 0; k < n; k++) {
    cJSON *o = booking_admin_json(db, &db->bookings[idx[k]]);
    if (!o) { free(idx); cJSON_Delete(arr); api_json(res, 500, NULL); return; }
    cJSON_AddItemToArray(arr, o);
  }
  free(idx);
  api_json(res, 200, arr);
}

static bool read_booking_fields(const cJSON *body, booking_t *b) {
  return body && api_get_int(body, "car_id", &b->car_id) && api_get_int(body, "user_id", &b->user_id) &&
         api_get_int(body, "start", &b->booked_start) && api_get_int(body, "end", &b->booked_end);
}

void h_admin_booking_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  booking_t b;
  memset(&b, 0, sizeof b);
  if (!read_booking_fields(ctx->body, &b)) { api_invalid(res); return; }
  b.state = BK_RESERVED;
  br_result_t r = br_check_create(&a->db, ctx->user, &b, req->now);
  if (r != BR_OK) { api_br_error(res, r); return; }
  b.created_at = b.modified_at = req->now;
  b.modified_by = ctx->user->id;
  if (db_put_booking(&a->db, &b) != 0) { api_storage_error(res); return; }
  api_json(res, 201, booking_admin_json(&a->db, &b));
}

void h_admin_booking_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/bookings/", &id)) { api_invalid(res); return; }
  const booking_t *old_p = db_booking(&a->db, id);
  if (!old_p) { api_not_found(res); return; }
  booking_t old = *old_p, nb = old;
  if (!read_booking_fields(ctx->body, &nb)) { api_invalid(res); return; }
  br_result_t r = br_check_update(&a->db, ctx->user, &old, &nb, req->now);
  if (r != BR_OK) { api_br_error(res, r); return; }
  nb.modified_at = req->now;
  nb.modified_by = ctx->user->id;
  if (db_put_booking(&a->db, &nb) != 0) { api_storage_error(res); return; }
  api_json(res, 200, booking_admin_json(&a->db, &nb));
}

void h_admin_booking_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/bookings/", &id)) { api_invalid(res); return; }
  const booking_t *old_p = db_booking(&a->db, id);
  if (!old_p) { api_not_found(res); return; }
  booking_t b = *old_p;
  br_result_t r = br_check_cancel(ctx->user, &b, req->now);
  if (r != BR_OK) { api_br_error(res, r); return; }
  b.state = BK_CANCELLED;
  b.cancelled_at = req->now;
  b.cancel_fee_cents = 0;
  b.modified_at = req->now;
  b.modified_by = ctx->user->id;
  if (db_put_booking(&a->db, &b) != 0) { api_storage_error(res); return; }
  api_json(res, 200, booking_admin_json(&a->db, &b));
}

/* ---- settings ---- */

static void settings_response(api_res_t *res, const settings_t *s) {
  cJSON *o = cJSON_CreateObject();
  if (!o || !cJSON_AddNumberToObject(o, "start_tolerance_min", s->start_tolerance_min) ||
      !cJSON_AddNumberToObject(o, "cancel_deadline_min", s->cancel_deadline_min) ||
      !cJSON_AddNumberToObject(o, "cancel_fee_cents", (double)s->cancel_fee_cents)) {
    cJSON_Delete(o);
    api_json(res, 500, NULL);
    return;
  }
  api_json(res, 200, o);
}

void h_admin_settings_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  (void)ctx;
  settings_response(res, &a->db.settings);
}

void h_admin_settings_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  int64_t tol, dl, fee;
  if (!ctx->body || !api_get_int(ctx->body, "start_tolerance_min", &tol) ||
      !api_get_int(ctx->body, "cancel_deadline_min", &dl) ||
      !api_get_int(ctx->body, "cancel_fee_cents", &fee) || tol < 0 || tol > INT_MAX || dl < 0 ||
      dl > INT_MAX || fee < 0 || fee > FEE_MAX) {
    api_invalid(res);
    return;
  }
  settings_t s = {(int)tol, (int)dl, fee};
  if (db_put_settings(&a->db, &s) != 0) { api_storage_error(res); return; }
  settings_response(res, &s);
}
