#include "http/igloo_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/igloo.h"

#define BUDGET_S 15
#define TOKEN_MARGIN_S 300
#define TOKEN_DEFAULT_TTL_S 3600
#define SMALL_ANSWER 8192
#define LIST_ANSWER 65536

void igloo_client_init(igloo_client_t *c, const config_t *cfg, igloo_http_fn http, int64_t (*now)(void)) {
  memset(c, 0, sizeof *c);
  c->cfg = cfg;
  c->http = http;
  c->now = now;
}

typedef struct { igloo_client_t *c; int64_t start; int last_status; } call_t;

/* Remaining budget in seconds; <= 0 means exhausted. */
static long remaining(const call_t *k) { return (long)(BUDGET_S - (k->c->now() - k->start)); }

static pin_err_t map_status(int st) {
  if (st >= 200 && st < 300) return PIN_OK;
  switch (st) {
    case 402: return PIN_ERR_PAYMENT;
    case 403: return PIN_ERR_SCOPE;
    case 404: return PIN_ERR_DEVICE;
    case 409: return PIN_ERR_TIMEZONE;
    case 418: return PIN_ERR_UNSUPPORTED;
    default: return PIN_ERR_OTHER;
  }
}

/* One HTTP exchange inside the budget. PIN_OK = answer received, *status set. */
static pin_err_t exchange(call_t *k, http_req_t *r, int *status, char *out, size_t cap) {
  long left = remaining(k);
  if (left <= 0) { k->last_status = 0; return PIN_ERR_NETWORK; }
  r->timeout_s = left;
  *status = 0;
  if (k->c->http(r, status, out, cap) != 0) { k->last_status = 0; return PIN_ERR_NETWORK; }
  k->last_status = *status;
  return PIN_OK;
}

static pin_err_t fetch_token(call_t *k) {
  igloo_client_t *c = k->c;
  int64_t t = c->now();
  if (c->token[0] && t < c->token_exp - TOKEN_MARGIN_S) return PIN_OK;
  c->token[0] = 0;
  c->token_exp = 0;
  http_req_t r = {.method = "POST", .url = c->cfg->igloo_token_url,
                  .basic_user = c->cfg->igloo_client_id, .basic_pass = c->cfg->igloo_client_secret,
                  .content_type = "application/x-www-form-urlencoded",
                  .body = "grant_type=client_credentials"};
  char *ans = malloc(SMALL_ANSWER);
  if (!ans) return PIN_ERR_OTHER;
  int st;
  pin_err_t e = exchange(k, &r, &st, ans, SMALL_ANSWER);
  if (e == PIN_OK) {
    if (st == 400 || st == 401) e = PIN_ERR_AUTH;
    else if (st < 200 || st >= 300) e = PIN_ERR_OTHER;
    else {
      int64_t ttl = 0;
      if (igloo_parse_token(ans, c->token, sizeof c->token, &ttl) != 0) e = PIN_ERR_OTHER;
      else {
        c->token_exp = t + (ttl > 0 ? ttl : TOKEN_DEFAULT_TTL_S);
      }
    }
  }
  memset(ans, 0, SMALL_ANSWER);
  free(ans);
  return e;
}

/* Authenticated call with token fetch and one 401 retry. PIN_OK = 2xx answer in ans. */
static pin_err_t api_call(call_t *k, http_req_t *r, char *ans, size_t cap) {
  igloo_client_t *c = k->c;
  for (int attempt = 0; attempt < 2; attempt++) {
    pin_err_t e = fetch_token(k);
    if (e != PIN_OK) return e;
    r->bearer = c->token;
    int st;
    e = exchange(k, r, &st, ans, cap);
    if (e != PIN_OK) return e;
    if (st == 401) {
      c->token[0] = 0;
      c->token_exp = 0;
      continue;
    }
    return map_status(st);
  }
  return PIN_ERR_AUTH;
}

static int configured(const igloo_client_t *c) {
  return c->cfg->igloo_client_id[0] && c->cfg->igloo_client_secret[0] && c->cfg->igloo_api_url[0] &&
         c->cfg->igloo_token_url[0];
}

pin_err_t igloo_pin_create(void *ctx, const char *device_id, int64_t from, int64_t until,
                           int variance, const char *access_name, pin_result_t *out) {
  igloo_client_t *c = ctx;
  out->http_status = 0;
  if (!configured(c)) return PIN_ERR_CONFIG;
  if (!lock_device_id_valid(device_id)) return PIN_ERR_DEVICE;
  char *body = igloo_hourly_body(from, until, variance, access_name);
  if (!body) return PIN_ERR_OTHER;
  char url[512];
  int n = snprintf(url, sizeof url, "%s/home/devices/%s/algopin/hourly", c->cfg->igloo_api_url, device_id);
  char *ans = malloc(SMALL_ANSWER);
  if (n < 0 || (size_t)n >= sizeof url || !ans) {
    free(body);
    free(ans);
    return PIN_ERR_OTHER;
  }
  call_t k = {c, c->now(), 0};
  http_req_t r = {.method = "POST", .url = url, .content_type = "application/json", .body = body};
  pin_err_t e = api_call(&k, &r, ans, SMALL_ANSWER);
  out->http_status = k.last_status;
  if (e == PIN_OK && igloo_parse_pin(ans, out) != 0) e = PIN_ERR_OTHER;
  /* parse_pin may have set fields; keep http_status as the HTTP answer */
  out->http_status = k.last_status;
  if (e != PIN_OK) { out->pin[0] = 0; out->pin_id[0] = 0; }
  memset(ans, 0, SMALL_ANSWER);
  free(ans);
  free(body);
  return e;
}

pin_err_t igloo_lock_list(void *ctx, lock_info_t *out, size_t cap, size_t *n, int *http_status) {
  igloo_client_t *c = ctx;
  *n = 0;
  if (http_status) *http_status = 0;
  if (!configured(c)) return PIN_ERR_CONFIG;
  char url[512];
  int u = snprintf(url, sizeof url, "%s/home/devices", c->cfg->igloo_api_url);
  char *ans = malloc(LIST_ANSWER);
  if (u < 0 || (size_t)u >= sizeof url || !ans) { free(ans); return PIN_ERR_OTHER; }
  call_t k = {c, c->now(), 0};
  http_req_t r = {.method = "GET", .url = url};
  pin_err_t e = api_call(&k, &r, ans, LIST_ANSWER);
  if (http_status) *http_status = k.last_status;
  if (e == PIN_OK && igloo_parse_devices(ans, out, cap, n) != 0) { *n = 0; e = PIN_ERR_OTHER; }
  free(ans);
  return e;
}
