#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/session.h"
#include "core/booking_rules.h"
#include "core/codec.h"
#include "core/email.h"
#include "core/sepa.h"
#include "http/api_internal.h"

/* Largest value the codec writes exactly (see MAX_WRITE_INT in core/codec.c). */
#define FEE_MAX ((int64_t)999999999999999)

/* ---- helpers ---- */

static void err_invalid_email(api_res_t *res) {
  api_error(res, 400, "invalid_email", "Ungültige E-Mail-Adresse.");
}
static void err_duplicate_email(api_res_t *res) {
  api_error(res, 409, "duplicate_email", "Diese E-Mail-Adresse ist bereits registriert.");
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
static bool read_user_fields(const cJSON *body, int64_t now, user_t *u, api_res_t *res) {
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
    if (!api_get_int(body, "license_checked_at", &v) || !br_time_in_range(v, now)) { api_invalid(res); return false; }
    u->license_checked_at = v;
  }
  const cJSON *mc = cJSON_GetObjectItemCaseSensitive(body, "may_club_trip");
  if (mc && !api_get_bool(body, "may_club_trip", &u->may_club_trip)) { api_invalid(res); return false; }
  if (email_normalize(email, u->email) != 0) { err_invalid_email(res); return false; }
  snprintf(u->name, sizeof u->name, "%s", name);
  snprintf(u->street, sizeof u->street, "%s", street);
  snprintf(u->zip, sizeof u->zip, "%s", zip);
  snprintf(u->city, sizeof u->city, "%s", city);
  return true;
}

void h_admin_user_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  user_t u;
  memset(&u, 0, sizeof u);
  if (!read_user_fields(ctx->body, req->now, &u, res)) return;
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
  if (!read_user_fields(ctx->body, req->now, &u, res)) return;
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
  if (!api_segment_id(req->path, "/api/admin/users/", &id)) { api_invalid(res); return; }
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

/* tracker_imei is optional: absent keeps the value already in c, "" clears it. */
static bool read_car_fields(db_t *db, const cJSON *body, car_t *c, api_res_t *res) {
  const char *name = body ? api_get_str(body, "name", sizeof c->name) : NULL;
  const char *plate = body ? api_get_str(body, "plate", sizeof c->plate) : NULL;
  const char *loc = body ? api_get_str(body, "location", sizeof c->location) : NULL;
  bool active;
  if (!name || !plate || !loc || !*name || !*plate || !api_get_bool(body, "active", &active)) {
    api_invalid(res);
    return false;
  }
  char imei[sizeof c->tracker_imei];
  snprintf(imei, sizeof imei, "%s", c->tracker_imei);
  if (body && cJSON_GetObjectItemCaseSensitive(body, "tracker_imei")) {
    const char *v = api_get_str(body, "tracker_imei", sizeof imei);
    if (!v || (*v && !imei_valid(v))) {
      api_invalid(res);
      return false;
    }
    snprintf(imei, sizeof imei, "%s", v);
  }
  if (*imei) {
    const car_t *other = db_car_by_imei(db, imei);
    if (other && other->id != c->id) {
      api_error(res, 409, "duplicate_imei", "Diese IMEI ist bereits einem Auto zugeordnet.");
      return false;
    }
  }
  snprintf(c->tracker_imei, sizeof c->tracker_imei, "%s", imei);
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
  if (!read_car_fields(&a->db, ctx->body, &c, res)) return;
  if (db_put_car(&a->db, &c) != 0) { api_storage_error(res); return; }
  api_json(res, 201, car_to_json(&c));
}

void h_admin_car_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/cars/", &id)) { api_invalid(res); return; }
  const car_t *old = db_car(&a->db, id);
  if (!old) { api_not_found(res); return; }
  car_t c = *old;
  if (!read_car_fields(&a->db, ctx->body, &c, res)) return;
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
static cJSON *booking_admin_json(app_t *a, const booking_t *b) {
  const db_t *db = &a->db;
  cJSON *o = api_booking_json(a, b);
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
    cJSON *o = booking_admin_json(a, &db->bookings[idx[k]]);
    if (!o) { free(idx); cJSON_Delete(arr); api_json(res, 500, NULL); return; }
    cJSON_AddItemToArray(arr, o);
  }
  free(idx);
  api_json(res, 200, arr);
}

/* club_trip is optional: absent keeps the value already in b. */
static bool read_booking_fields(const cJSON *body, booking_t *b) {
  if (!body || !api_get_int(body, "car_id", &b->car_id) || !api_get_int(body, "user_id", &b->user_id) ||
      !api_get_int(body, "start", &b->booked_start) || !api_get_int(body, "end", &b->booked_end))
    return false;
  if (cJSON_GetObjectItemCaseSensitive(body, "club_trip") && !api_get_bool(body, "club_trip", &b->club_trip))
    return false;
  return true;
}

/* actual_start/actual_end: integer or null (= 0), absent keeps b. km_start/km_end: both or neither. */
static bool read_trip_fields(const cJSON *body, booking_t *b, bool *has_actual) {
  static const char *const keys[2] = {"actual_start", "actual_end"};
  int64_t *const dst[2] = {&b->actual_start, &b->actual_end};
  for (int i = 0; i < 2; i++) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, keys[i]);
    if (!v) continue;
    *has_actual = true;
    if (cJSON_IsNull(v)) *dst[i] = 0;
    else if (!api_get_int(body, keys[i], dst[i])) return false;
  }
  bool has_ks = cJSON_GetObjectItemCaseSensitive(body, "km_start") != NULL;
  bool has_ke = cJSON_GetObjectItemCaseSensitive(body, "km_end") != NULL;
  if (has_ks != has_ke) return false;
  if (has_ks) {
    int64_t ks, ke;
    if (!api_get_int(body, "km_start", &ks) || !api_get_int(body, "km_end", &ke)) return false;
    if (ks < 0 || ke < ks || ke > 10000000) return false;
    b->km_start = ks;
    b->km_end = ke;
    b->km_source = KM_MANUAL;
    b->km_pending = false;
  }
  return true;
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
  api_json(res, 201, booking_admin_json(a, &b));
}

void h_admin_booking_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t id;
  if (!api_path_id(req->path, "/api/admin/bookings/", &id)) { api_invalid(res); return; }
  const booking_t *old_p = db_booking(&a->db, id);
  if (!old_p) { api_not_found(res); return; }
  booking_t old = *old_p, nb = old;
  bool has_actual = false;
  if (!read_booking_fields(ctx->body, &nb) || !read_trip_fields(ctx->body, &nb, &has_actual)) {
    api_invalid(res);
    return;
  }
  br_result_t r = BR_OK;
  /* A cancelled booking is left to br_check_update (not_modifiable); apply_trip would revive it. */
  if (has_actual && old.state != BK_CANCELLED) r = br_apply_trip(&old, &nb, req->now);
  if (r != BR_OK) { api_br_error(res, r); return; }
  r = br_check_update(&a->db, ctx->user, &old, &nb, req->now);
  if (r != BR_OK) { api_br_error(res, r); return; }
  nb.modified_at = req->now;
  nb.modified_by = ctx->user->id;
  if (db_put_booking(&a->db, &nb) != 0) { api_storage_error(res); return; }
  api_json(res, 200, booking_admin_json(a, &nb));
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
  api_json(res, 200, booking_admin_json(a, &b));
}

/* ---- settings ---- */

static void settings_response(api_res_t *res, const settings_t *s) {
  cJSON *o = cJSON_CreateObject();
  if (!o || !cJSON_AddNumberToObject(o, "start_tolerance_min", s->start_tolerance_min) ||
      !cJSON_AddNumberToObject(o, "cancel_deadline_min", s->cancel_deadline_min) ||
      !cJSON_AddNumberToObject(o, "cancel_fee_cents", (double)s->cancel_fee_cents) ||
      !cJSON_AddStringToObject(o, "assoc_name", s->assoc_name) ||
      !cJSON_AddStringToObject(o, "assoc_street", s->assoc_street) ||
      !cJSON_AddStringToObject(o, "assoc_zip", s->assoc_zip) ||
      !cJSON_AddStringToObject(o, "assoc_city", s->assoc_city) ||
      !cJSON_AddStringToObject(o, "creditor_id", s->creditor_id) ||
      !cJSON_AddStringToObject(o, "creditor_iban", s->creditor_iban) ||
      !cJSON_AddStringToObject(o, "creditor_bic", s->creditor_bic)) {
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
  settings_t s = a->db.settings;
  const char *name = api_get_str(ctx->body, "assoc_name", sizeof s.assoc_name);
  const char *street = api_get_str(ctx->body, "assoc_street", sizeof s.assoc_street);
  const char *zip = api_get_str(ctx->body, "assoc_zip", sizeof s.assoc_zip);
  const char *city = api_get_str(ctx->body, "assoc_city", sizeof s.assoc_city);
  const char *cid = api_get_str(ctx->body, "creditor_id", 256);
  const char *iban = api_get_str(ctx->body, "creditor_iban", 256);
  const char *bic = api_get_str(ctx->body, "creditor_bic", 256);
  if (!name || !street || !zip || !city || !cid || !iban || !bic) { api_invalid(res); return; }
  if (sepa_normalize(cid, s.creditor_id, sizeof s.creditor_id) != 0 ||
      (*s.creditor_id && !creditor_id_valid(s.creditor_id))) {
    api_error(res, 400, "invalid_creditor_id", "Ungültige Gläubiger-ID.");
    return;
  }
  if (sepa_normalize(iban, s.creditor_iban, sizeof s.creditor_iban) != 0 ||
      (*s.creditor_iban && !iban_valid(s.creditor_iban))) {
    api_error(res, 400, "invalid_iban", "Die IBAN ist ungültig.");
    return;
  }
  if (sepa_normalize(bic, s.creditor_bic, sizeof s.creditor_bic) != 0 ||
      (*s.creditor_bic && !bic_valid(s.creditor_bic))) {
    api_error(res, 400, "invalid_bic", "Die BIC ist ungültig.");
    return;
  }
  memcpy(s.assoc_name, name, strlen(name) + 1);
  memcpy(s.assoc_street, street, strlen(street) + 1);
  memcpy(s.assoc_zip, zip, strlen(zip) + 1);
  memcpy(s.assoc_city, city, strlen(city) + 1);
  s.start_tolerance_min = (int)tol;
  s.cancel_deadline_min = (int)dl;
  s.cancel_fee_cents = fee;
  if (db_put_settings(&a->db, &s) != 0) { api_storage_error(res); return; }
  settings_response(res, &s);
}
