/* Auth endpoints: login, logout, password forgot/set, /api/me. */
#include <stdio.h>
#include <string.h>

#include "auth/password.h"
#include "auth/session.h"
#include "http/api_internal.h"

#define COOKIE_ATTRS "HttpOnly; Secure; SameSite=Strict; Path=/"

static void bad_credentials(api_res_t *res, int status) {
  api_error(res, status, "bad_credentials", "E-Mail oder Passwort ist falsch.");
}

static void too_short(api_res_t *res) {
  api_error(res, 400, "password_too_short", "Das Passwort muss mindestens 10 Zeichen lang sein.");
}

/* Password field: string of at most API_PW_MAX_BYTES bytes, else NULL. */
static const char *get_pw(const cJSON *body, const char *key) {
  return api_get_str(body, key, API_PW_MAX_BYTES + 1);
}

/* Verifies against a fixed dummy hash so unknown and known emails cost the same. */
static void burn_verify(const char *pw) {
  static char dummy[160];
  static int ready;
  if (!ready) ready = pw_hash("ocs-dummy-password", dummy) == 0 ? 1 : -1;
  if (ready == 1) (void)pw_verify(pw, dummy);
}

void h_login(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  user_t probe;
  const char *email = api_get_str(ctx->body, "email", sizeof probe.email);
  const char *pw = get_pw(ctx->body, "password");
  if (!email || !pw) {
    api_invalid(res);
    return;
  }

  char ip_key[160], email_key[160];
  snprintf(ip_key, sizeof ip_key, "ip:%s", req->client_ip ? req->client_ip : "");
  int n = snprintf(email_key, sizeof email_key, "email:%s", email);
  for (int i = 6; i < n && i < (int)sizeof email_key; i++)
    if (email_key[i] >= 'A' && email_key[i] <= 'Z') email_key[i] = (char)(email_key[i] - 'A' + 'a');
  if (!rl_allowed(&a->rl, ip_key, req->now) || !rl_allowed(&a->rl, email_key, req->now)) {
    api_error(res, 429, "rate_limited", "Zu viele Fehlversuche. Bitte versuche es später erneut.");
    return;
  }

  const user_t *u = db_user_by_email(&a->db, email);
  bool ok;
  if (u && u->active && u->pw_hash[0]) {
    ok = pw_verify(pw, u->pw_hash);
  } else {
    burn_verify(pw);
    ok = false;
  }
  if (!ok) {
    rl_fail(&a->rl, ip_key, req->now);
    rl_fail(&a->rl, email_key, req->now);
    bad_credentials(res, 401);
    return;
  }
  rl_reset(&a->rl, ip_key);
  rl_reset(&a->rl, email_key);

  user_t copy = *u;
  char tok[65];
  if (session_create(a, copy.id, req->now, tok) != 0) {
    api_storage_error(res);
    return;
  }
  snprintf(res->set_cookie, sizeof res->set_cookie, "ocs_session=%s; " COOKIE_ATTRS "; Max-Age=%d",
           tok, SESSION_TTL_SECS);
  api_user_response(res, 200, &copy);
}

void api_clear_session_cookie(api_res_t *res) {
  snprintf(res->set_cookie, sizeof res->set_cookie, "ocs_session=; " COOKIE_ATTRS "; Max-Age=0");
}

void h_logout(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  if (req->session_token && session_destroy(a, req->session_token) != 0) {
    api_storage_error(res);
    return;
  }
  api_clear_session_cookie(res);
  api_ok(res);
}

/* Reset mails: at most FORGOT_MAX_SENDS per RL_WINDOW_SECS per client IP and
 * per (lowercased) email, so a member cannot be mail-bombed and SMTP cannot
 * stall the event loop at will. Separate keys from the login limit. */
#define FORGOT_MAX_SENDS 3

static void lower_from(char *s, int from, int n, size_t cap) {
  for (int i = from; i < n && i < (int)cap; i++)
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] - 'A' + 'a');
}

void h_password_forgot(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  user_t probe;
  const char *email = api_get_str(ctx->body, "email", sizeof probe.email);
  if (!email) {
    api_invalid(res);
    return;
  }
  char ip_key[160], email_key[160];
  snprintf(ip_key, sizeof ip_key, "forgot:ip:%s", req->client_ip ? req->client_ip : "");
  int n = snprintf(email_key, sizeof email_key, "forgot:email:%s", email);
  lower_from(email_key, 13, n, sizeof email_key);
  /* Always 200: the response must not reveal whether the address exists or
   * whether the request was throttled; token/mail failures are swallowed too. */
  if (!rl_allowed_n(&a->rl, ip_key, req->now, FORGOT_MAX_SENDS) ||
      !rl_allowed_n(&a->rl, email_key, req->now, FORGOT_MAX_SENDS)) {
    api_ok(res);
    return;
  }
  rl_fail_n(&a->rl, ip_key, req->now, FORGOT_MAX_SENDS);
  rl_fail_n(&a->rl, email_key, req->now, FORGOT_MAX_SENDS);
  const user_t *u = db_user_by_email(&a->db, email);
  if (u && u->active) (void)api_send_token_mail(a, u->id, TOKEN_RESET, req->now);
  api_ok(res);
}

void h_password_set(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const cJSON *tv = cJSON_GetObjectItemCaseSensitive(ctx->body, "token");
  const char *pw = get_pw(ctx->body, "password");
  if (!cJSON_IsString(tv) || !pw) {
    api_invalid(res);
    return;
  }
  /* Checked before consuming, so a too-short attempt does not burn the token. */
  if (api_utf8_len(pw) < API_PW_MIN_CHARS) {
    too_short(res);
    return;
  }
  int64_t uid = token_consume(a, tv->valuestring, req->now);
  const user_t *u = uid > 0 ? db_user(&a->db, uid) : NULL;
  if (!u) {
    api_error(res, 400, "invalid_token", "Der Link ist ungültig oder abgelaufen.");
    return;
  }
  user_t c = *u;
  /* Devices registered through an old session stop getting reminders; each registers again
   * after its next login. */
  if (pw_hash(pw, c.pw_hash) != 0 || db_put_user(&a->db, &c) != 0 ||
      db_delete_sessions(&a->db, NULL, c.id) != 0 || db_delete_push_subs_of(&a->db, c.id) != 0) {
    api_storage_error(res);
    return;
  }
  api_ok(res);
}

void h_me_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)a;
  (void)req;
  api_user_response(res, 200, ctx->user);
}

void h_me_put(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  user_t c = *ctx->user;
  /* Only the name is self-service; the postal address is admin-only (§4). */
  const char *name = api_get_str(ctx->body, "name", sizeof c.name);
  if (!name) {
    api_invalid(res);
    return;
  }
  snprintf(c.name, sizeof c.name, "%s", name);
  if (db_put_user(&a->db, &c) != 0) {
    api_storage_error(res);
    return;
  }
  api_user_response(res, 200, &c);
}

void h_me_password(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const char *cur = get_pw(ctx->body, "current");
  const char *nw = get_pw(ctx->body, "new");
  if (!cur || !nw) {
    api_invalid(res);
    return;
  }
  user_t c = *ctx->user;
  if (!c.pw_hash[0] || !pw_verify(cur, c.pw_hash)) {
    bad_credentials(res, 400);
    return;
  }
  if (api_utf8_len(nw) < API_PW_MIN_CHARS) {
    too_short(res);
    return;
  }
  if (pw_hash(nw, c.pw_hash) != 0 || db_put_user(&a->db, &c) != 0) {
    api_storage_error(res);
    return;
  }
  /* Destroy all sessions of the user, then restore the current one. */
  char h[65];
  session_hash(req->session_token, h);
  session_t keep;
  bool have = false;
  for (size_t i = 0; i < a->db.n_sessions; i++)
    if (strcmp(a->db.sessions[i].token_hash, h) == 0) {
      keep = a->db.sessions[i];
      have = true;
    }
  /* Push rows go with the sessions; the client re-registers this device right after. */
  if (db_delete_sessions(&a->db, NULL, c.id) != 0 || (have && db_put_session(&a->db, &keep) != 0) ||
      db_delete_push_subs_of(&a->db, c.id) != 0) {
    api_storage_error(res);
    return;
  }
  api_ok(res);
}
