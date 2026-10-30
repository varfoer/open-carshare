#ifndef OCS_HTTP_API_INTERNAL_H
#define OCS_HTTP_API_INTERNAL_H

/* Shared by api.c and the endpoint files (api_auth.c, api_booking.c, api_admin.c).
 * To add an endpoint: declare its handler here, implement it in the endpoint
 * file, and add a row to ROUTES in api.c. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"
#include "cJSON.h"
#include "core/model.h"
#include "http/api.h"

typedef enum { MIN_PUBLIC = 0, MIN_USER, MIN_ADMIN } min_role_t;

typedef struct {
  /* Copy of the authenticated user (stable across db writes); NULL on PUBLIC routes. */
  const user_t *user;
  /* Parsed request body: a JSON object, or NULL if the body was empty or the
   * method is GET. Owned by api_handle. */
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
/* 200/201 {"user":<public json>} */
void api_user_response(api_res_t *res, int status, const user_t *u);

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
void h_settings_public(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);

#endif
