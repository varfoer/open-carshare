#include "http/api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "auth/session.h"
#include "core/booking_rules.h"
#include "core/codec.h"
#include "http/api_internal.h"

/* ---- route table ----
 * Patterns: '*' matches exactly one non-empty path segment (no '/').
 * Tasks 8/9: add rows here; declare handlers in http/api_internal.h. */
typedef struct {
  const char *method, *pattern;
  api_handler_fn handler;
  min_role_t min_role;
} route_t;

static const route_t ROUTES[] = {
    {"POST", "/api/login", h_login, MIN_PUBLIC},
    {"POST", "/api/logout", h_logout, MIN_PUBLIC},
    {"POST", "/api/password/forgot", h_password_forgot, MIN_PUBLIC},
    {"POST", "/api/password/set", h_password_set, MIN_PUBLIC},
    {"GET", "/api/me", h_me_get, MIN_USER},
    {"PUT", "/api/me", h_me_put, MIN_USER},
    {"PUT", "/api/me/password", h_me_password, MIN_USER},
    {"GET", "/api/cars", h_cars_get, MIN_USER},
    {"GET", "/api/calendar", h_calendar_get, MIN_USER},
    {"GET", "/api/bookings", h_bookings_get, MIN_USER},
    {"POST", "/api/bookings", h_booking_post, MIN_USER},
    {"PUT", "/api/bookings/*", h_booking_put, MIN_USER},
    {"DELETE", "/api/bookings/*", h_booking_delete, MIN_USER},
    {"GET", "/api/settings/public", h_settings_public, MIN_USER},
    {"GET", "/api/admin/users", h_admin_users_get, MIN_ADMIN},
    {"POST", "/api/admin/users", h_admin_user_post, MIN_ADMIN},
    {"PUT", "/api/admin/users/*", h_admin_user_put, MIN_ADMIN},
    {"POST", "/api/admin/users/*/invite", h_admin_user_invite, MIN_ADMIN},
    {"GET", "/api/admin/cars", h_admin_cars_get, MIN_ADMIN},
    {"POST", "/api/admin/cars", h_admin_car_post, MIN_ADMIN},
    {"PUT", "/api/admin/cars/*", h_admin_car_put, MIN_ADMIN},
    {"GET", "/api/admin/bookings", h_admin_bookings_get, MIN_ADMIN},
    {"POST", "/api/admin/bookings", h_admin_booking_post, MIN_ADMIN},
    {"PUT", "/api/admin/bookings/*", h_admin_booking_put, MIN_ADMIN},
    {"DELETE", "/api/admin/bookings/*", h_admin_booking_delete, MIN_ADMIN},
    {"GET", "/api/admin/settings", h_admin_settings_get, MIN_ADMIN},
    {"PUT", "/api/admin/settings", h_admin_settings_put, MIN_ADMIN},
};

static bool pattern_match(const char *pat, const char *path) {
  while (*pat) {
    if (*pat == '*') {
      if (!*path || *path == '/') return false;
      while (*path && *path != '/') path++;
      pat++;
    } else {
      if (*pat != *path) return false;
      pat++;
      path++;
    }
  }
  return *path == 0;
}

/* ---- responses ---- */

void api_res_free(api_res_t *res) {
  if (!res) return;
  free(res->body);
  res->body = NULL;
}

static void set_body(api_res_t *res, int status, char *body) {
  free(res->body);
  res->status = status;
  res->body = body;
}

void api_error(api_res_t *res, int status, const char *code, const char *msg_de) {
  cJSON *j = cJSON_CreateObject();
  char *s = NULL;
  if (j && cJSON_AddStringToObject(j, "error", code) && cJSON_AddStringToObject(j, "message", msg_de))
    s = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  set_body(res, status, s);
}

void api_json(api_res_t *res, int status, cJSON *body) {
  char *s = body ? cJSON_PrintUnformatted(body) : NULL;
  cJSON_Delete(body);
  if (!s) {
    api_error(res, 500, "internal", "Interner Fehler.");
    return;
  }
  set_body(res, status, s);
}

void api_ok(api_res_t *res) {
  cJSON *j = cJSON_CreateObject();
  if (j && !cJSON_AddTrueToObject(j, "ok")) {
    cJSON_Delete(j);
    j = NULL;
  }
  api_json(res, 200, j);
}

void api_invalid(api_res_t *res) { api_error(res, 400, "invalid", "Ungültige Eingabe."); }
void api_storage_error(api_res_t *res) { api_error(res, 500, "storage", "Speichern fehlgeschlagen."); }
void api_not_found(api_res_t *res) { api_error(res, 404, "not_found", "Nicht gefunden."); }

void api_br_error(api_res_t *res, int r) {
  switch ((br_result_t)r) {
    case BR_INVALID: api_error(res, 400, "invalid", "Ungültige Eingabe."); break;
    case BR_IN_PAST: api_error(res, 400, "in_past", "Der Beginn liegt in der Vergangenheit."); break;
    case BR_CAR_INACTIVE:
      api_error(res, 400, "car_inactive", "Dieses Auto ist derzeit nicht buchbar.");
      break;
    case BR_OVERLAP:
      api_error(res, 409, "overlap", "Das Auto ist in diesem Zeitraum bereits gebucht.");
      break;
    case BR_NOT_EXTENSION:
      api_error(res, 400, "only_extend",
                "Buchungen können nur verlängert werden. Zum Verkürzen bitte stornieren und neu buchen.");
      break;
    case BR_NOT_MODIFIABLE:
      api_error(res, 400, "not_modifiable", "Diese Buchung kann nicht mehr geändert werden.");
      break;
    case BR_CLUB_FORBIDDEN:
      api_error(res, 403, "forbidden", "Du darfst keine Vereinsfahrten buchen.");
      break;
    case BR_FORBIDDEN:
    default: api_not_found(res); break;
  }
}

void api_user_response(api_res_t *res, int status, const user_t *u) {
  cJSON *j = cJSON_CreateObject();
  cJSON *pu = user_to_public_json(u);
  if (!j || !pu || !cJSON_AddItemToObject(j, "user", pu)) {
    cJSON_Delete(j);
    cJSON_Delete(pu);
    api_json(res, status, NULL);
    return;
  }
  api_json(res, status, j);
}

/* ---- parsing helpers ---- */

/* Digits only, at least one, no overflow. */
static bool parse_digits(const char *s, size_t n, int64_t *out) {
  if (n == 0) return false;
  int64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
    int d = s[i] - '0';
    if (v > (INT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  *out = v;
  return true;
}

bool api_path_id(const char *path, const char *prefix, int64_t *id) {
  if (!path || !prefix || !id) return false;
  size_t pl = strlen(prefix);
  if (strncmp(path, prefix, pl) != 0) return false;
  const char *rest = path + pl;
  if (pl == 0 || prefix[pl - 1] != '/') {
    if (*rest != '/') return false;
    rest++;
  }
  return parse_digits(rest, strlen(rest), id);
}

bool api_query_int(const char *query, const char *key, int64_t *out) {
  if (!query || !key || !out) return false;
  size_t kl = strlen(key);
  const char *p = query;
  while (*p) {
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
      const char *v = p + kl + 1;
      size_t vl = len - kl - 1;
      bool neg = vl > 0 && v[0] == '-';
      int64_t x;
      if (!parse_digits(v + neg, vl - neg, &x)) return false;
      *out = neg ? -x : x;
      return true;
    }
    if (!end) break;
    p = end + 1;
  }
  return false;
}

const char *api_get_str(const cJSON *obj, const char *key, size_t cap) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsString(v) || !v->valuestring) return NULL;
  if (strlen(v->valuestring) >= cap) return NULL;
  return v->valuestring;
}

bool api_get_int(const cJSON *obj, const char *key, int64_t *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsNumber(v)) return false;
  double d = v->valuedouble;
  /* 2^63 is exactly representable; anything >= it overflows int64. */
  if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) return false;
  int64_t i = (int64_t)d;
  if ((double)i != d) return false;
  *out = i;
  return true;
}

bool api_get_bool(const cJSON *obj, const char *key, bool *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  if (!cJSON_IsBool(v)) return false;
  *out = cJSON_IsTrue(v);
  return true;
}

size_t api_utf8_len(const char *s) {
  size_t n = 0;
  for (; *s; s++)
    if (((unsigned char)*s & 0xC0) != 0x80) n++;
  return n;
}

/* ---- mail ---- */

int api_send_mail(app_t *a, const char *to, const char *subject, const char *body) {
  if (!a->mail_send) return -1;
  return a->mail_send(a->mail_ctx, to, subject, body) == 0 ? 0 : -1;
}

int api_send_token_mail(app_t *a, int64_t user_id, token_kind_t kind, int64_t now) {
  const user_t *u = db_user(&a->db, user_id);
  if (!u) return -2;
  char to[sizeof u->email], name[sizeof u->name];
  snprintf(to, sizeof to, "%s", u->email);
  snprintf(name, sizeof name, "%s", u->name);
  /* u may dangle after the put below; only the copies are used from here on. */
  char tok[65];
  if (token_issue(a, user_id, kind, now, tok) != 0) return -1;
  size_t cap = 1024 + sizeof a->cfg.base_url + sizeof name;
  char *body = malloc(cap);
  if (!body) return -3;
  const char *subject;
  if (kind == TOKEN_INVITE) {
    subject = "Einladung zu open-carshare";
    snprintf(body, cap,
             "Hallo %s,\n\n"
             "du wurdest zu open-carshare eingeladen. Über den folgenden Link legst du dein "
             "Passwort fest:\n\n%s/#/passwort?token=%s\n\n"
             "Der Link ist 7 Tage gültig.\n",
             name, a->cfg.base_url, tok);
  } else {
    subject = "Passwort zurücksetzen";
    snprintf(body, cap,
             "Hallo %s,\n\n"
             "über den folgenden Link kannst du ein neues Passwort festlegen:\n\n"
             "%s/#/passwort?token=%s\n\n"
             "Der Link ist eine Stunde gültig. Falls du das nicht angefordert hast, "
             "ignoriere diese E-Mail einfach.\n",
             name, a->cfg.base_url, tok);
  }
  int rc = api_send_mail(a, to, subject, body);
  free(body);
  return rc == 0 ? 0 : -3;
}

/* ---- dispatch ---- */

static bool has_json_content_type(const char *ct) {
  static const char want[] = "application/json";
  size_t n = sizeof want - 1;
  if (!ct || strncasecmp(ct, want, n) != 0) return false;
  char c = ct[n];
  return c == 0 || c == ';' || c == ' ' || c == '\t';
}

static bool is_body_method(const char *m) {
  return strcmp(m, "POST") == 0 || strcmp(m, "PUT") == 0 || strcmp(m, "DELETE") == 0;
}

/* NULL + *ok=false: invalid JSON or not an object. NULL + *ok=true: empty body. */
static cJSON *parse_body(const api_req_t *req, bool *ok) {
  *ok = true;
  if (!req->body || req->body_len == 0) return NULL;
  *ok = false;
  if (memchr(req->body, 0, req->body_len)) return NULL;
  char *buf = malloc(req->body_len + 1);
  if (!buf) return NULL;
  memcpy(buf, req->body, req->body_len);
  buf[req->body_len] = 0;
  const char *end = NULL;
  cJSON *j = cJSON_ParseWithOpts(buf, &end, 1);
  free(buf);
  if (!cJSON_IsObject(j)) {
    cJSON_Delete(j);
    return NULL;
  }
  *ok = true;
  return j;
}

size_t api_route_count(void) { return sizeof ROUTES / sizeof ROUTES[0]; }
const char *api_route_method(size_t i) { return i < api_route_count() ? ROUTES[i].method : NULL; }
const char *api_route_pattern(size_t i) { return i < api_route_count() ? ROUTES[i].pattern : NULL; }

static void dispatch(app_t *a, const api_req_t *req, api_res_t *res) {
  if (!req->method || !req->path) {
    api_error(res, 400, "invalid", "Ungültige Anfrage.");
    return;
  }
  const route_t *route = NULL;
  bool path_known = false;
  for (size_t i = 0; i < sizeof ROUTES / sizeof ROUTES[0]; i++) {
    if (!pattern_match(ROUTES[i].pattern, req->path)) continue;
    path_known = true;
    if (strcmp(ROUTES[i].method, req->method) == 0) {
      route = &ROUTES[i];
      break;
    }
  }
  if (!route) {
    if (path_known)
      api_error(res, 405, "method_not_allowed", "Methode nicht erlaubt.");
    else
      api_not_found(res);
    return;
  }

  bool body_method = is_body_method(req->method);
  if (body_method && !has_json_content_type(req->content_type)) {
    api_error(res, 415, "unsupported_media_type", "Content-Type muss application/json sein.");
    return;
  }

  user_t user; /* copy: stays valid across db writes in the handler */
  api_ctx_t ctx = {NULL, NULL};
  if (route->min_role != MIN_PUBLIC) {
    const user_t *u = session_user(a, req->session_token, req->now);
    if (!u) {
      api_error(res, 401, "unauthenticated", "Bitte melde dich an.");
      return;
    }
    if (route->min_role == MIN_ADMIN && u->role != ROLE_ADMIN) {
      api_error(res, 403, "forbidden", "Keine Berechtigung.");
      return;
    }
    user = *u;
    ctx.user = &user;
  }

  cJSON *body = NULL;
  if (body_method) {
    bool ok;
    body = parse_body(req, &ok);
    if (!ok) {
      api_error(res, 400, "invalid_json", "Ungültiges JSON.");
      return;
    }
  }
  ctx.body = body;
  route->handler(a, req, &ctx, res);
  cJSON_Delete(body);
}

void api_handle(app_t *a, const api_req_t *req, api_res_t *res) {
  memset(res, 0, sizeof *res);
  dispatch(a, req, res);
  if (res->status == 0) {
    api_error(res, 500, "internal", "Interner Fehler.");
  } else if (!res->body) {
    res->body = malloc(3);
    if (res->body) memcpy(res->body, "{}", 3);
  }
}
