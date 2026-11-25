#ifndef OCS_HTTP_API_H
#define OCS_HTTP_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "cJSON.h"
#include "core/notify.h"

/* All strings borrowed for the duration of api_handle. body need not be
 * NUL-terminated; body_len is authoritative. query excludes the '?'. */
typedef struct {
  const char *method, *path, *query, *body, *content_type, *session_token, *client_ip;
  size_t body_len; int64_t now;
} api_req_t;

/* body: malloc'd JSON (NULL only if even the error body could not be
 * allocated). set_cookie: empty or a full Set-Cookie header value.
 * content_type: empty = application/json; otherwise body is binary and
 * body_len is its length (0 = strlen(body)). */
typedef struct {
  int status; char *body; char set_cookie[256];
  char content_type[32]; size_t body_len;
} api_res_t;

void api_handle(app_t *a, const api_req_t *req, api_res_t *res);
void api_res_free(api_res_t *res);

/* Largest accepted request body for method+path: 2097152 on JPEG upload
 * routes, 65536 otherwise (unknown routes included). api_handle answers
 * 413 too_large above it; the HTTP adapter checks it before reading the body. */
size_t api_body_limit(const char *method, const char *path);

/* Route table introspection (fuzzers, docs). Patterns use '*' for one path segment. */
size_t api_route_count(void);
const char *api_route_method(size_t i);
const char *api_route_pattern(size_t i);

/* Closes every RESERVED booking with booked_end <= now (never started) as DONE without
 * actual times. Returns the number closed, -1 on storage error. */
int api_expire_bookings(app_t *a, int64_t now);

/* Resolves km_pending DONE bookings of cars with tracker_imei (spec §8). Sets km, km_source
 * TELEMATICS, km_pending false, modified_at now, modified_by 0. Returns number resolved, -1 storage error. */
int api_resolve_km(app_t *a, int64_t now);
/* Daily mail to all active admins listing km_pending bookings of tracker cars with
 * now - actual_end > 86400. 1 sent, 0 nothing to send or already sent this UTC day, -1 all sends failed. */
int api_km_pending_mail(app_t *a, int64_t now);

/* Mails the booking's user about kind (NT_CREATED/MODIFIED/CANCELLED). actor_id: who acted.
 * 0 sent, 1 skipped (user missing or inactive), -1 build or send failed. Never touches the db. */
int api_notify_booking(app_t *a, notify_kind_t kind, const booking_t *b, int64_t actor_id);

/* Sends due start/end/late reminders (rules: spec §10) and persists the flag after each
 * successful send: by push when the user has subscriptions and at least one push is delivered,
 * else by mail; the late-return reminder always also by mail (its flag follows the mail). Returns number sent, -1 on storage error (stops). Mail failures are skipped
 * (flag stays false, retried next call) and end the call, so a down relay costs one SMTP timeout
 * per call. */
int api_send_reminders(app_t *a, int64_t now);

/* Mails the "billing run done" text to every active admin. 1 if at least one sent, 0 no admin,
 * -1 all failed (or the text does not fit). */
int api_notify_run_sent(app_t *a, const billing_run_t *run, const invoice_t *invs, size_t n,
                        size_t sent_now, const int64_t *failed, size_t n_failed);

/* Mails "Fahrt beendet: <car>" with the local return time (actual_end) to every active admin, so
 * they can check the charger. 1 if at least one sent, 0 no admin, -1 all failed. */
int api_notify_returned(app_t *a, const booking_t *b);

/* Loads the VAPID key from vapid.json, or creates and persists one; sets push_ready. 0 ok, -1 fail. */
int api_push_init(app_t *a);

/* helpers for api_*.c */
void api_json(api_res_t *res, int status, cJSON *body);           /* takes ownership; NULL -> 500 */
void api_error(api_res_t *res, int status, const char *code, const char *msg_de);
/* "/api/bookings/12" with prefix "/api/bookings/" (or "/api/bookings") -> 12.
 * Rejects empty, signs, non-digits, trailing chars and int64 overflow. */
bool api_path_id(const char *path, const char *prefix, int64_t *id);
/* Id of the path segment right after prefix (ends at '/' or end of path): "/api/bookings/12/start"
 * with prefix "/api/bookings/" -> 12. Same rejections as api_path_id. */
bool api_segment_id(const char *path, const char *prefix, int64_t *id);
/* Exact key match in "a=1&b=-2"; optional '-', digits only, no overflow. */
bool api_query_int(const char *query, const char *key, int64_t *out);
/* 0 ok, -1 if no mailer configured or sending failed. */
int  api_send_mail(app_t *a, const char *to, const char *subject, const char *body);
int  api_send_mail_att(app_t *a, const char *to, const char *subject, const char *body,
                       const mail_att_t *att);

#endif
