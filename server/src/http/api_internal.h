#ifndef OCS_HTTP_API_INTERNAL_H
#define OCS_HTTP_API_INTERNAL_H

/* Shared by api.c and the endpoint files (api_auth.c, api_booking.c, api_access.c, api_damage.c, api_admin.c, api_tariff.c, api_price.c, api_mandate.c, api_billing.c).
 * To add an endpoint: declare its handler here, implement it in the endpoint
 * file, and add a row to ROUTES in api.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "cJSON.h"
#include "core/model.h"
#include "core/pricing.h"
#include "http/api.h"

typedef enum { MIN_PUBLIC = 0, MIN_USER, MIN_ADMIN } min_role_t;

/* BODY_JPEG: Content-Type must be image/jpeg; the handler reads req->body raw. */
typedef enum { BODY_JSON = 0, BODY_JPEG } body_kind_t;

#define API_JSON_MAX_BYTES 65536
#define API_PHOTO_MAX_BYTES 2097152

typedef struct {
  /* Copy of the authenticated user (stable across db writes); NULL on PUBLIC routes. */
  const user_t *user;
  /* Parsed request body: a JSON object, or NULL if the body was empty, the
   * method is GET or the route is BODY_JPEG. Owned by api_handle. */
  const cJSON *body;
} api_ctx_t;

typedef void (*api_handler_fn)(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* Field readers. A string is accepted only if it is a JSON string with
 * strlen < cap (i.e. it fits a char[cap] field without truncation). */
const char *api_get_str(const cJSON *obj, const char *key, size_t cap);
bool api_get_int(const cJSON *obj, const char *key, int64_t *out);  /* integral number */
bool api_get_bool(const cJSON *obj, const char *key, bool *out);

/* Common responses. */
void api_ok(api_res_t *res);                       /* 200 {"ok":true} */
void api_invalid(api_res_t *res);                  /* 400 invalid */
void api_storage_error(api_res_t *res);            /* 500 storage */
void api_not_found(api_res_t *res);                /* 404 not_found */
/* Binary response; takes ownership of the malloc'd body. */
void api_binary(api_res_t *res, int status, const char *content_type, char *body, size_t len);
/* 200/201 {"user":<public json>} */
void api_user_response(api_res_t *res, int status, const user_t *u);

/* Map a br_result_t (core/booking_rules.h) to its HTTP error response. */
void api_br_error(api_res_t *res, int br_result);

/* Issue a reset/invite token for user_id and mail the link
 * <base_url>/#/passwort?token=<token>. 0 ok, -1 token/storage failure,
 * -2 user missing, -3 mail failure. */
int api_send_token_mail(app_t *a, int64_t user_id, token_kind_t kind, int64_t now);

/* Number of UTF-8 code points (counts non-continuation bytes). */
size_t api_utf8_len(const char *s);
#define API_PW_MIN_CHARS 10
#define API_PW_MAX_BYTES 1024

/* api_auth.c */
void h_login(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_logout(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_password_forgot(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_password_set(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_me_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_me_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_me_password(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* api_booking.c */
void h_cars_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_calendar_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_bookings_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_booking_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_booking_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_booking_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_booking_start(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_booking_return(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_settings_public(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* api_access.c (igloohome key box PINs) */
/* Creates a PIN with igloohome and stores its row. 0 ok (*row filled); -1 igloohome failed (*err);
 * -2 variance exhausted for car and valid_from; -3 storage. booking_id 0 = admin test. */
int api_access_create(app_t *a, int64_t car_id, const char *device_id, int64_t booking_id,
                      int64_t from, int64_t until, const char *access_name, int64_t now,
                      access_pin_t *row, pin_err_t *err);
/* Writes the 503 pin_unavailable response for rc -1/-2 of api_access_create. */
void api_pin_unavailable(api_res_t *res, int rc, int64_t valid_from);
/* api_access_create for booking b on device_id with access_window(now, b->booked_end) and the
 * access name "Buchung <id>". true with *row filled; false with 503/500 written to res. */
bool api_access_booking_pin(app_t *a, const booking_t *b, const char *device_id, int64_t now,
                            access_pin_t *row, api_res_t *res);
/* After a successful booking put: blanks the PIN digits of the booking when it left BK_ACTIVE or its
 * car_id or user_id changed. A failure is logged to stderr; the caller's response stays. */
void api_access_booking_changed(app_t *a, const booking_t *old, const booking_t *nb);
/* After a successful car put that changed lock_device_id: blanks the PIN digits of the car's active
 * bookings (they belong to the old box). A failure is logged to stderr; the caller's response stays. */
void api_access_lock_changed(app_t *a, int64_t car_id);
/* GET/POST /api/bookings/<id>/pin: owner only, booking active, car has a box; else 404. */
void h_booking_pin_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_booking_pin_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* api_damage.c */
void h_damage_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_damage_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_photo_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_photo_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* Admin box setup, MIN_ADMIN: GET /api/admin/locks (igloohome device list),
 * POST /api/admin/cars/<id>/lock-test (real test PIN for the current hour, stored hidden). */
void h_admin_locks_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_lock_test(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* api_admin.c (all MIN_ADMIN) */
void h_admin_users_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_user_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_user_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_user_invite(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
/* GDPR export of one user's personal records (spec §12/§13); full IBANs never included. */
void h_admin_user_export(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_cars_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_car_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_car_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_bookings_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_booking_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_booking_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_booking_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_settings_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_settings_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* api_tariff.c (all MIN_ADMIN) */
/* tariff_to_json plus "versions": the tariff's versions, newest valid_from first. NULL on OOM. */
cJSON *tariff_api_json(const db_t *db, const tariff_t *t);
void h_admin_tariffs_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_tariff_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_tariff_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_tariff_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_version_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_version_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
/* Assignments: GET/POST /api/admin/users/<id>/tariffs (list newest valid_from first),
 * PUT/DELETE /api/admin/user-tariffs/<id> (only the user's newest assignment). */
void h_admin_user_tariffs_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_user_tariff_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_user_tariff_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_user_tariff_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* api_billing.c (all MIN_ADMIN) */
void h_admin_billing_list(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_billing_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_billing_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_billing_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_billing_finalize(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_billing_send(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_billing_sepa(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_invoice_pdf(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* api_price.c */
/* booking_to_json plus "price" (tariff of the user at booked_start) and "live": null, or for an
 * active trip / a km_pending return of a car with a tracker {km, as_of, price}: km from the readings
 * so far (km_live), price as if returned now (active) or at actual_end. NULL on OOM. */
cJSON *api_booking_json(app_t *a, const booking_t *b, int64_t now);
/* Every price_t number by name, status as string, "tariff" = tariff_version_to_json(t) or null.
 * NULL on OOM. */
cJSON *price_to_json(const price_t *p, const tariff_version_t *t);
void h_price_estimate(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* api_mandate.c */
/* API view of a mandate: id, ref, status, holder, iban_masked, bic, created_at, ended_at,
 * ended_by, client_ip, text_version, creditor_name, creditor_id. Never the full IBAN. NULL on OOM. */
cJSON *mandate_api_json(const mandate_t *m);
void h_me_mandate_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_me_mandate_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
/* Ends every ACTIVE mandate of user_id as REVOKED (ended_by actor_id), cancels every RESERVED
 * booking of the user (cancel_fee_cents 0, cancelled_at/modified_at now, modified_by actor_id),
 * sets the user inactive and deletes the user's sessions, in this order.
 * 0 ok, -1 storage error (steps done so far stay), -2 no current mandate. */
int api_mandate_revoke(app_t *a, int64_t user_id, int64_t actor_id, int64_t now);
/* Cancels every RESERVED booking of user_id: cancel_fee_cents 0, state CANCELLED,
 * cancelled_at/modified_at now, modified_by actor_id. No mails. Count or -1 on storage error. */
int api_cancel_reserved(app_t *a, int64_t user_id, int64_t actor_id, int64_t now);
void h_me_mandate_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_user_mandates_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_admin_user_mandate_revoke(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
/* api_auth.c: sets the expiring ocs_session cookie (Max-Age=0) on res. */
void api_clear_session_cookie(api_res_t *res);

/* api_push.c (Web Push, spec §10). */
typedef enum { PUSH_SENT = 0, PUSH_GONE, PUSH_FAIL } push_result_t;
/* {"title","body","url","tag"} as a malloc'd string (free), NULL on OOM. */
char *api_push_payload(const char *title, const char *body, const char *url, const char *tag);
/* Encrypts payload for s and POSTs it to its push service. SENT: 2xx. GONE: 404/410, or the row
 * can never be delivered (endpoint outside the allow-list, undecodable key). FAIL: anything else,
 * crypto failures included. */
push_result_t api_push_one(app_t *a, const push_sub_t *s, const char *payload, int64_t now);
/* api_push_one to every subscription of user_id; GONE rows are deleted. Returns the number SENT;
 * *failed (if non-NULL) = the others. */
int api_push_user(app_t *a, int64_t user_id, const char *payload, int64_t now, int *failed);
bool api_push_has(const app_t *a, int64_t user_id);  /* at least one subscription */
/* GET /api/push/key; POST/DELETE /api/me/push {endpoint[, p256dh, auth]}; POST /api/me/push/test
 * (5 per user and 15 min, then 429 rate_limited). */
void h_push_key_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_me_push_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_me_push_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
void h_me_push_test(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

/* api_notify.c. Booking PUT, before the put: a changed booked_start re-arms the start reminder,
 * a changed booked_end the end and late reminders, a changed user_id all three. */
void api_booking_changed(const booking_t *old, booking_t *nb);
/* Booking PUT, after the put. user_id changed: NT_CANCELLED (old row) to the old user and
 * NT_CREATED to the new one. Else NT_MODIFIED if booked_start, booked_end, car_id or club_trip
 * differ; nothing otherwise. Results ignored. */
void api_notify_booking_update(app_t *a, const booking_t *old, const booking_t *nb, int64_t actor_id);

#endif
