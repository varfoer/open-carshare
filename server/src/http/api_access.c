#include <stdio.h>
#include <string.h>

#include "core/access.h"
#include "core/berlin.h"
#include "http/api_internal.h"

#define PIN_FAIL_HEAD "PIN für die Schlüsselbox konnte nicht abgerufen werden. "
#define PIN_FAIL_TAIL "Klappt es weiter nicht: Notfallkontakt auf der Hilfe-Seite."

/* Berlin wall clock of utc. */
static void berlin_hm(int64_t utc, int *h, int *m) {
  int64_t s = (utc + berlin_offset(utc)) % 86400;
  if (s < 0) s += 86400;
  *h = (int)(s / 3600);
  *m = (int)(s % 3600 / 60);
}

/* ---- variance attempts (builder call 3) ---- */

static pin_attempt_t *attempt_find(app_t *a, int64_t car_id, int64_t hour) {
  for (size_t i = 0; i < APP_PIN_ATTEMPTS; i++) {
    pin_attempt_t *t = &a->pin_attempts[i];
    if (t->used > 0 && t->car_id == car_id && t->hour == hour) return t;
  }
  return NULL;
}

/* Records used for (car, hour) in t, else in a free entry, else in the entry with the smallest hour. */
static void attempt_record(app_t *a, pin_attempt_t *t, int64_t car_id, int64_t hour, int used) {
  for (size_t i = 0; !t && i < APP_PIN_ATTEMPTS; i++)
    if (a->pin_attempts[i].used == 0) t = &a->pin_attempts[i];
  if (!t) {
    t = &a->pin_attempts[0];
    for (size_t i = 1; i < APP_PIN_ATTEMPTS; i++)
      if (a->pin_attempts[i].hour < t->hour) t = &a->pin_attempts[i];
  }
  t->car_id = car_id;
  t->hour = hour;
  t->used = used;
}

/* Non-empty and NUL-terminated within cap. */
static bool text_ok(const char *s, size_t cap) { return s[0] && memchr(s, 0, cap); }

int api_access_create(app_t *a, int64_t car_id, const char *device_id, int64_t booking_id,
                      int64_t from, int64_t until, const char *access_name, int64_t now,
                      access_pin_t *row, pin_err_t *err) {
  *err = PIN_OK;
  pin_attempt_t *t = attempt_find(a, car_id, from);
  int used = db_pin_max_variance(&a->db, car_id, from);
  if (t && t->used > used) used = t->used;
  if (used >= ACCESS_VARIANCE_MAX) return -2;
  int variance = used + 1;
  /* Before the call: a timeout may still have created the PIN at igloohome. */
  attempt_record(a, t, car_id, from, variance);

  pin_result_t out;
  memset(&out, 0, sizeof out);
  pin_err_t e = a->pin_create
      ? a->pin_create(a->pin_ctx, device_id, from, until, variance, access_name, &out)
      : PIN_ERR_CONFIG;
  if (e == PIN_OK && (!text_ok(out.pin, sizeof out.pin) || !text_ok(out.pin_id, sizeof out.pin_id)))
    e = PIN_ERR_OTHER;
  if (e != PIN_OK) {
    fprintf(stderr, "ocs-server: pin for booking %lld failed: %s (HTTP %d)\n", (long long)booking_id,
            pin_err_log(e), out.http_status);
    *err = e;
    return -1;
  }
  access_pin_t p;
  memset(&p, 0, sizeof p);
  p.booking_id = booking_id;
  p.car_id = car_id;
  snprintf(p.pin_id, sizeof p.pin_id, "%s", out.pin_id);
  /* Test PINs (booking 0) are stored hidden; the digits only go to the caller. */
  if (booking_id != 0) snprintf(p.pin, sizeof p.pin, "%s", out.pin);
  p.valid_from = from;
  p.valid_until = until;
  p.variance = variance;
  p.created_at = now;
  if (db_put_pin(&a->db, &p) != 0) return -3;
  *row = p;
  if (booking_id == 0) snprintf(row->pin, sizeof row->pin, "%s", out.pin);
  return 0;
}

void api_pin_unavailable(api_res_t *res, int rc, int64_t valid_from) {
  if (rc != -2) {
    api_error(res, 503, "pin_unavailable", PIN_FAIL_HEAD "Bitte erneut versuchen. " PIN_FAIL_TAIL);
    return;
  }
  /* The next hour gives a new valid_from and fresh variances. */
  int h, m;
  berlin_hm(valid_from + 3600, &h, &m);
  char msg[256];
  snprintf(msg, sizeof msg, PIN_FAIL_HEAD "Bitte ab %02d:00 erneut versuchen. " PIN_FAIL_TAIL, h);
  api_error(res, 503, "pin_unavailable", msg);
}

bool api_access_booking_pin(app_t *a, const booking_t *b, const char *device_id, int64_t now,
                            access_pin_t *row, api_res_t *res) {
  int64_t from, until;
  access_window(now, b->booked_end, &from, &until);
  char name[32];
  snprintf(name, sizeof name, "Buchung %lld", (long long)b->id);
  pin_err_t err;
  int rc = api_access_create(a, b->car_id, device_id, b->id, from, until, name, now, row, &err);
  if (rc == 0) return true;
  if (rc == -3)
    api_storage_error(res);
  else
    api_pin_unavailable(res, rc, from);
  return false;
}

void api_access_booking_changed(app_t *a, const booking_t *old, const booking_t *nb) {
  bool left_active = old->state == BK_ACTIVE && nb->state != BK_ACTIVE;
  if (!left_active && old->car_id == nb->car_id && old->user_id == nb->user_id) return;
  if (db_pins_blank(&a->db, nb->id) != 0)
    fprintf(stderr, "ocs-server: blanking the pins of booking %lld failed\n", (long long)nb->id);
}

/* ---- endpoints ---- */

/* The caller's own active booking by path id whose car has a key box, and that box's device ID.
 * false with 400/404 in res otherwise; admins get no override (design: owner only). */
static bool pin_booking(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res,
                        booking_t *b, char device[64]) {
  int64_t id;
  if (!api_segment_id(req->path, "/api/bookings/", &id)) { api_invalid(res); return false; }
  const booking_t *p = db_booking(&a->db, id);
  const car_t *c = p ? db_car(&a->db, p->car_id) : NULL;
  if (!p || p->user_id != ctx->user->id || p->state != BK_ACTIVE || !c || !c->lock_device_id[0]) {
    api_not_found(res);
    return false;
  }
  _Static_assert(sizeof c->lock_device_id == 64, "device buffer");
  *b = *p;
  memcpy(device, c->lock_device_id, sizeof c->lock_device_id);
  return true;
}

/* {pin, valid_from, valid_until} of p; pin null unless usable, zeros without p. */
static void pin_response(api_res_t *res, const access_pin_t *p, int64_t now) {
  cJSON *o = cJSON_CreateObject();
  bool ok = o != NULL;
  if (ok && p && access_usable(p, now))
    ok = cJSON_AddStringToObject(o, "pin", p->pin) != NULL;
  else if (ok)
    ok = cJSON_AddNullToObject(o, "pin") != NULL;
  ok = ok && cJSON_AddNumberToObject(o, "valid_from", p ? (double)p->valid_from : 0) &&
       cJSON_AddNumberToObject(o, "valid_until", p ? (double)p->valid_until : 0);
  if (!ok) {
    cJSON_Delete(o);
    api_json(res, 500, NULL);
    return;
  }
  api_json(res, 200, o);
}

void h_booking_pin_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  booking_t b;
  char device[64];
  if (!pin_booking(a, req, ctx, res, &b, device)) return;
  pin_response(res, db_pin_newest(&a->db, b.id), req->now);
}

void h_booking_pin_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  booking_t b;
  char device[64];
  if (!pin_booking(a, req, ctx, res, &b, device)) return;
  bool renew = false;
  if (ctx->body && cJSON_GetObjectItemCaseSensitive(ctx->body, "renew") &&
      !api_get_bool(ctx->body, "renew", &renew)) {
    api_invalid(res);
    return;
  }
  const access_pin_t *newest = db_pin_newest(&a->db, b.id);
  if (newest && !renew && access_usable(newest, req->now)) {
    pin_response(res, newest, req->now);
    return;
  }
  if (newest && renew && newest->created_at + ACCESS_RENEW_GAP_S > req->now) {
    /* Rounded up to the minute, so the named time is never too early. */
    int64_t t = newest->created_at + ACCESS_RENEW_GAP_S;
    t += (60 - t % 60) % 60;
    int h, m;
    berlin_hm(t, &h, &m);
    char msg[64];
    snprintf(msg, sizeof msg, "Eine neue PIN ist ab %02d:%02d möglich.", h, m);
    api_error(res, 429, "pin_too_soon", msg);
    return;
  }
  access_pin_t row;
  if (!api_access_booking_pin(a, &b, device, req->now, &row, res)) return;
  pin_response(res, &row, req->now);
}

/* ---- admin box setup ---- */

void h_admin_locks_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  (void)ctx;
  lock_info_t l[32];
  size_t n = 0;
  int http = 0;
  pin_err_t e = a->lock_list ? a->lock_list(a->pin_ctx, l, 32, &n, &http) : PIN_ERR_CONFIG;
  if (e != PIN_OK) {
    fprintf(stderr, "ocs-server: igloo device list failed: %s (HTTP %d)\n", pin_err_log(e), http);
    api_error(res, 503, "locks_unavailable", pin_err_admin_de(e));
    return;
  }
  if (n > 32) n = 32;
  cJSON *arr = cJSON_CreateArray();
  if (!arr) { api_json(res, 500, NULL); return; }
  for (size_t i = 0; i < n; i++) {
    cJSON *o = cJSON_CreateObject();
    if (!o || !cJSON_AddStringToObject(o, "device_id", l[i].device_id) ||
        !cJSON_AddStringToObject(o, "name", l[i].name)) {
      cJSON_Delete(o);
      cJSON_Delete(arr);
      api_json(res, 500, NULL);
      return;
    }
    cJSON_AddItemToArray(arr, o);
  }
  api_json(res, 200, arr);
}

void h_admin_lock_test(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  int64_t id;
  if (!api_segment_id(req->path, "/api/admin/cars/", &id)) { api_invalid(res); return; }
  const car_t *c = db_car(&a->db, id);
  if (!c) { api_not_found(res); return; }
  if (!c->lock_device_id[0]) {
    api_error(res, 400, "no_lock", "Für dieses Auto ist keine Schlüsselbox eingetragen.");
    return;
  }
  char device[64];
  memcpy(device, c->lock_device_id, sizeof device);
  int64_t from = req->now - ((req->now % 3600) + 3600) % 3600;
  int64_t until = from + 3600;
  char name[48];
  snprintf(name, sizeof name, "Test Auto %lld", (long long)id);
  access_pin_t row;
  pin_err_t err;
  int rc = api_access_create(a, id, device, 0, from, until, name, req->now, &row, &err);
  if (rc == -3) { api_storage_error(res); return; }
  if (rc == -1) { api_error(res, 503, "lock_test_failed", pin_err_admin_de(err)); return; }
  if (rc == -2) {
    int h, m;
    berlin_hm(until, &h, &m);
    char msg[128];
    snprintf(msg, sizeof msg, "Für diese Stunde sind keine Test-PINs mehr möglich. Bitte ab %02d:00 erneut versuchen.", h);
    api_error(res, 503, "lock_test_failed", msg);
    return;
  }
  cJSON *o = cJSON_CreateObject();
  if (!o || !cJSON_AddStringToObject(o, "pin", row.pin) ||
      !cJSON_AddNumberToObject(o, "valid_until", (double)row.valid_until)) {
    cJSON_Delete(o);
    api_json(res, 500, NULL);
    return;
  }
  api_json(res, 200, o);
}
