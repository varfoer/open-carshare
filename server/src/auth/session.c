#include "auth/session.h"

#include <string.h>

#include "monocypher.h"
#include "platform/platform.h"

static void hex(const uint8_t *in, size_t n, char *out) {
  static const char d[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = d[in[i] >> 4];
    out[2 * i + 1] = d[in[i] & 15];
  }
  out[2 * n] = 0;
}

/* Only tokens we could have issued: exactly 64 lowercase hex chars. */
static int token_wellformed(const char *t) {
  if (!t) return 0;
  size_t i = 0;
  for (; t[i]; i++) {
    if (i >= 64) return 0;
    if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f'))) return 0;
  }
  return i == 64;
}

void session_hash(const char *token, char out[65]) {
  uint8_t h[32];
  crypto_blake2b(h, sizeof h, (const uint8_t *)token, strlen(token));
  hex(h, sizeof h, out);
  crypto_wipe(h, sizeof h);
}

static int new_token(char token_out[65], char hash_out[65]) {
  uint8_t r[32];
  if (platform_random(r, sizeof r) != 0) return -1;
  hex(r, sizeof r, token_out);
  crypto_wipe(r, sizeof r);
  session_hash(token_out, hash_out);
  return 0;
}

int session_create(app_t *a, int64_t user_id, int64_t now, char token_out[65]) {
  session_t s;
  memset(&s, 0, sizeof s);
  token_out[0] = 0;
  (void)db_prune(&a->db, now); /* best effort: keep the tables from growing forever */
  char tok[65];
  if (new_token(tok, s.token_hash) != 0) return -1;
  s.user_id = user_id;
  s.expires_at = now + SESSION_TTL_SECS;
  if (db_put_session(&a->db, &s) != 0) {
    crypto_wipe(tok, sizeof tok);
    return -1;
  }
  memcpy(token_out, tok, sizeof tok);
  crypto_wipe(tok, sizeof tok);
  return 0;
}

user_t *session_user(app_t *a, const char *token, int64_t now) {
  if (!token_wellformed(token)) return NULL;
  char h[65];
  session_hash(token, h);
  for (size_t i = 0; i < a->db.n_sessions; i++) {
    const session_t *s = &a->db.sessions[i];
    if (strcmp(s->token_hash, h) != 0) continue;
    if (now >= s->expires_at) return NULL;
    user_t *u = db_user(&a->db, s->user_id);
    return (u && u->active) ? u : NULL;
  }
  return NULL;
}

int session_destroy(app_t *a, const char *token) {
  if (!token_wellformed(token)) return 0;
  char h[65];
  session_hash(token, h);
  return db_delete_sessions(&a->db, h, 0);
}

int token_issue(app_t *a, int64_t user_id, token_kind_t kind, int64_t now, char token_out[65]) {
  token_t t;
  memset(&t, 0, sizeof t);
  token_out[0] = 0;
  (void)db_prune(&a->db, now);
  char tok[65];
  if (new_token(tok, t.token_hash) != 0) return -1;
  t.user_id = user_id;
  t.kind = kind;
  t.expires_at = now + (kind == TOKEN_INVITE ? TOKEN_INVITE_TTL_SECS : TOKEN_RESET_TTL_SECS);
  t.used = false;
  if (db_put_token(&a->db, &t) != 0) {
    crypto_wipe(tok, sizeof tok);
    return -1;
  }
  memcpy(token_out, tok, sizeof tok);
  crypto_wipe(tok, sizeof tok);
  return 0;
}

int64_t token_consume(app_t *a, const char *token, int64_t now) {
  if (!token_wellformed(token)) return -1;
  char h[65];
  session_hash(token, h);
  const token_t *t = db_token(&a->db, h);
  if (!t || t->used || now >= t->expires_at) return -1;
  token_t c = *t; /* copy: db_put_token needs it for rollback */
  c.used = true;
  if (db_put_token(&a->db, &c) != 0) return -1;
  return c.user_id;
}
