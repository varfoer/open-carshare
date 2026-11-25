/* Web Push (spec §10): VAPID key, subscriptions of the logged-in user, and sending encrypted
 * messages to the browsers' push services. Sends are synchronous, like mail. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/push.h"
#include "http/api_internal.h"
#include "http/webpush.h"

#define PUSH_TIMEOUT_S 5
#define PUSH_TTL "3600"
#define VAPID_EXP_S (12 * 3600)
#define PUSH_TEST_MAX 5 /* test messages per user and RL_WINDOW_SECS */

int api_push_init(app_t *a) {
  uint8_t priv[32];
  size_t n = 0;
  if (!a->db.vapid_key[0]) {
    char b64[48];
    if (platform_p256_keygen(priv, a->vapid_pub) != 0 || b64url_encode(priv, 32, b64, sizeof b64) != 0 ||
        db_put_vapid_key(&a->db, b64) != 0)
      return -1;
  }
  if (b64url_decode(a->db.vapid_key, priv, sizeof priv, &n) != 0 || n != 32 ||
      platform_p256_public(priv, a->vapid_pub) != 0 ||
      b64url_encode(a->vapid_pub, 65, a->vapid_pub_b64, sizeof a->vapid_pub_b64) != 0)
    return -1;
  memcpy(a->vapid_priv, priv, 32);
  a->push_ready = true;
  return 0;
}

char *api_push_payload(const char *title, const char *body, const char *url, const char *tag) {
  cJSON *o = cJSON_CreateObject();
  char *s = NULL;
  if (o && cJSON_AddStringToObject(o, "title", title) && cJSON_AddStringToObject(o, "body", body) &&
      cJSON_AddStringToObject(o, "url", url) && cJSON_AddStringToObject(o, "tag", tag))
    s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  return s;
}

/* sub claim: base_url if https, else mailto:<mail_from> if set, else none. */
static const char *vapid_sub(const app_t *a, char *buf, size_t cap) {
  if (strncmp(a->cfg.base_url, "https://", 8) == 0) return a->cfg.base_url;
  if (!a->cfg.mail_from[0]) return NULL;
  int k = snprintf(buf, cap, "mailto:%s", a->cfg.mail_from);
  return k < 0 || (size_t)k >= cap ? NULL : buf;
}

push_result_t api_push_one(app_t *a, const push_sub_t *s, const char *payload, int64_t now) {
  if (!a->push_ready || !a->https) return PUSH_FAIL;
  char aud[PUSH_ENDPOINT_MAX];
  uint8_t ua_pub[65], auth[16], as_priv[32], as_pub[65], salt[16];
  size_t np = 0, na = 0;
  /* Rows that no longer pass the checks (allow-list changed, bad key) can never be delivered. */
  if (push_audience(s->endpoint, aud, sizeof aud) != 0 ||
      b64url_decode(s->p256dh, ua_pub, sizeof ua_pub, &np) != 0 || np != 65 ||
      b64url_decode(s->auth, auth, sizeof auth, &na) != 0 || na != 16)
    return PUSH_GONE;
  size_t len = strlen(payload), body_len = 0;
  uint8_t body[WEBPUSH_MAX_PLAIN + WEBPUSH_OVERHEAD];
  if (platform_p256_keygen(as_priv, as_pub) != 0 || platform_random(salt, sizeof salt) != 0) return PUSH_FAIL;
  /* FAIL, not GONE: a crypto failure (e.g. out of memory) must not delete a valid row; the browser
   * key was checked on the curve when the row was stored. */
  if (webpush_encrypt(ua_pub, auth, as_priv, salt, (const uint8_t *)payload, len, body, sizeof body,
                      &body_len) != 0)
    return PUSH_FAIL;
  char vapid[1400], sub_buf[160];
  if (webpush_vapid_header(a->vapid_priv, a->vapid_pub, aud, vapid_sub(a, sub_buf, sizeof sub_buf),
                           now + VAPID_EXP_S, vapid, sizeof vapid) != 0)
    return PUSH_FAIL;
  const char *headers[] = {vapid, "TTL: " PUSH_TTL, "Urgency: high", "Content-Encoding: aes128gcm", NULL};
  http_req_t r = {.method = "POST", .url = s->endpoint, .content_type = "application/octet-stream",
                  .body = (const char *)body, .body_len = body_len, .headers = headers,
                  .truncate_answer = true, .timeout_s = PUSH_TIMEOUT_S};
  char out[512];
  int status = 0;
  if (a->https(&r, &status, out, sizeof out) != 0) {
    fprintf(stderr, "push: no answer from %s\n", aud);
    return PUSH_FAIL;
  }
  if (status >= 200 && status < 300) return PUSH_SENT;
  if (status == 404 || status == 410) return PUSH_GONE;
  fprintf(stderr, "push: HTTP %d from %s\n", status, aud);
  return PUSH_FAIL;
}

int api_push_user(app_t *a, int64_t user_id, const char *payload, int64_t now, int *failed) {
  /* Copy first: sends do not touch the table, deletes afterwards do. */
  push_sub_t subs[PUSH_MAX_PER_USER];
  size_t n = 0;
  for (size_t i = 0; i < a->db.n_subs && n < PUSH_MAX_PER_USER; i++)
    if (a->db.subs[i].user_id == user_id) subs[n++] = a->db.subs[i];
  int sent = 0, bad = 0;
  for (size_t i = 0; i < n; i++) {
    push_result_t r = api_push_one(a, &subs[i], payload, now);
    if (r == PUSH_SENT) {
      sent++;
      continue;
    }
    bad++;
    if (r == PUSH_GONE && db_delete_push_sub(&a->db, subs[i].endpoint, 0) != 0)
      fprintf(stderr, "push: cannot delete a gone subscription\n");
  }
  if (failed) *failed = bad;
  return sent;
}

bool api_push_has(const app_t *a, int64_t user_id) {
  for (size_t i = 0; i < a->db.n_subs; i++)
    if (a->db.subs[i].user_id == user_id) return true;
  return false;
}

void h_push_key_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  (void)ctx;
  if (!a->push_ready) {
    api_error(res, 503, "push_unavailable", "Push-Benachrichtigungen sind nicht verfügbar.");
    return;
  }
  cJSON *o = cJSON_CreateObject();
  if (o && !cJSON_AddStringToObject(o, "key", a->vapid_pub_b64)) {
    cJSON_Delete(o);
    o = NULL;
  }
  api_json(res, 200, o);
}

void h_me_push_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  push_sub_t s;
  memset(&s, 0, sizeof s);
  const char *ep = api_get_str(ctx->body, "endpoint", sizeof s.endpoint);
  const char *p = api_get_str(ctx->body, "p256dh", sizeof s.p256dh);
  const char *au = api_get_str(ctx->body, "auth", sizeof s.auth);
  uint8_t key[65], auth[16], tmp[32];
  size_t nk = 0, na = 0;
  if (!ep || !p || !au || b64url_decode(p, key, sizeof key, &nk) != 0 || nk != 65 ||
      b64url_decode(au, auth, sizeof auth, &na) != 0 || na != 16 || !a->push_ready ||
      platform_p256_ecdh(a->vapid_priv, key, tmp) != 0) { /* also rejects points off the curve */
    api_invalid(res);
    return;
  }
  if (!push_endpoint_ok(ep)) {
    api_error(res, 400, "push_endpoint", "Dieser Push-Dienst wird nicht unterstützt.");
    return;
  }
  snprintf(s.endpoint, sizeof s.endpoint, "%s", ep);
  snprintf(s.p256dh, sizeof s.p256dh, "%s", p);
  snprintf(s.auth, sizeof s.auth, "%s", au);
  s.user_id = ctx->user->id;
  s.created_at = req->now;
  if (db_put_push_sub(&a->db, &s) != 0) {
    api_storage_error(res);
    return;
  }
  api_ok(res);
}

void h_me_push_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  const char *ep = api_get_str(ctx->body, "endpoint", PUSH_ENDPOINT_MAX);
  if (!ep) {
    api_invalid(res);
    return;
  }
  if (db_delete_push_sub(&a->db, ep, ctx->user->id) != 0) {
    api_storage_error(res);
    return;
  }
  api_ok(res);
}

void h_me_push_test(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  /* Each call blocks the event loop for up to 5 sends: a few per user and 15 min. */
  char key[48];
  snprintf(key, sizeof key, "push-test:%lld", (long long)ctx->user->id);
  if (!rl_allowed_n(&a->rl, key, req->now, PUSH_TEST_MAX)) {
    api_error(res, 429, "rate_limited", "Zu viele Testnachrichten. Bitte versuche es später erneut.");
    return;
  }
  rl_fail_n(&a->rl, key, req->now, PUSH_TEST_MAX);
  char *payload = api_push_payload("Testnachricht", "Push-Benachrichtigungen sind eingerichtet.", "/#/konto", "test");
  if (!payload) {
    api_json(res, 500, NULL);
    return;
  }
  int failed = 0;
  int sent = api_push_user(a, ctx->user->id, payload, req->now, &failed);
  free(payload);
  cJSON *o = cJSON_CreateObject();
  if (o && (!cJSON_AddNumberToObject(o, "sent", sent) || !cJSON_AddNumberToObject(o, "failed", failed))) {
    cJSON_Delete(o);
    o = NULL;
  }
  api_json(res, 200, o);
}
