#include "auth/password.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "monocypher.h"
#include "platform/platform.h"

#define PW_M 19456u
#define PW_T 2u
#define PW_P 1u
#define SALT_LEN 16
#define HASH_LEN 32
#define MAX_M 65536u
#define MAX_T 10u

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_enc(const uint8_t *in, size_t n, char *out) {
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = B64[(v >> 18) & 63];
    out[o++] = B64[(v >> 12) & 63];
    if (i + 1 < n) out[o++] = B64[(v >> 6) & 63];
    if (i + 2 < n) out[o++] = B64[v & 63];
  }
  out[o] = 0;
  return o;
}

/* Decode exactly `len` chars of unpadded b64 into out (cap bytes). Returns byte count or -1. */
static int b64_dec(const char *in, size_t len, uint8_t *out, size_t cap) {
  if (len % 4 == 1) return -1;
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++) {
    const char *p = memchr(B64, in[i], 64);
    if (!p || in[i] == 0) return -1;
    acc = (acc << 6) | (uint32_t)(p - B64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (uint8_t)(acc >> bits);
      acc &= (1u << bits) - 1;
    }
  }
  if (acc != 0) return -1; /* non-canonical trailing bits */
  return (int)o;
}

static int derive(const char *pw, const uint8_t salt[SALT_LEN], uint32_t m, uint32_t t,
                  uint32_t p, uint8_t hash[HASH_LEN]) {
  void *work = malloc((size_t)m * 1024);
  if (!work) return -1;
  crypto_argon2_config cfg = {CRYPTO_ARGON2_ID, m, t, p};
  crypto_argon2_inputs in = {(const uint8_t *)pw, salt, (uint32_t)strlen(pw), SALT_LEN};
  crypto_argon2(hash, HASH_LEN, work, cfg, in, crypto_argon2_no_extras);
  free(work); /* crypto_argon2 wipes its work area */
  return 0;
}

int pw_hash(const char *pw, char out[160]) {
  uint8_t salt[SALT_LEN], hash[HASH_LEN];
  if (platform_random(salt, sizeof salt) != 0) return -1;
  if (derive(pw, salt, PW_M, PW_T, PW_P, hash) != 0) return -1;
  char s[32], h[64];
  b64_enc(salt, sizeof salt, s);
  b64_enc(hash, sizeof hash, h);
  crypto_wipe(hash, sizeof hash);
  int n = snprintf(out, 160, "$argon2id$v=19$m=%u,t=%u,p=%u$%s$%s", PW_M, PW_T, PW_P, s, h);
  return (n > 0 && n < 160) ? 0 : -1;
}

/* Parse unsigned decimal at *s, stop at first non-digit. */
static bool parse_u(const char **s, uint32_t *v) {
  const char *p = *s;
  uint64_t x = 0;
  if (*p < '0' || *p > '9') return false;
  while (*p >= '0' && *p <= '9') {
    x = x * 10 + (uint64_t)(*p - '0');
    if (x > 0xFFFFFFFFu) return false;
    p++;
  }
  *v = (uint32_t)x;
  *s = p;
  return true;
}

static bool lit(const char **s, const char *l) {
  size_t n = strlen(l);
  if (strncmp(*s, l, n) != 0) return false;
  *s += n;
  return true;
}

bool pw_verify(const char *pw, const char *encoded) {
  const char *s = encoded;
  uint32_t m, t, p;
  if (!lit(&s, "$argon2id$v=19$m=") || !parse_u(&s, &m)) return false;
  if (!lit(&s, ",t=") || !parse_u(&s, &t)) return false;
  if (!lit(&s, ",p=") || !parse_u(&s, &p) || !lit(&s, "$")) return false;
  if (m < 8 || m > MAX_M || t < 1 || t > MAX_T || p != 1) return false;

  const char *sep = strchr(s, '$');
  if (!sep) return false;
  uint8_t salt[SALT_LEN], want[HASH_LEN], got[HASH_LEN];
  if (b64_dec(s, (size_t)(sep - s), salt, sizeof salt) != SALT_LEN) return false;
  s = sep + 1;
  if (b64_dec(s, strlen(s), want, sizeof want) != HASH_LEN) return false;

  if (derive(pw, salt, m, t, p, got) != 0) {
    crypto_wipe(want, sizeof want);
    return false;
  }
  bool ok = crypto_verify32(got, want) == 0;
  crypto_wipe(got, sizeof got);
  crypto_wipe(want, sizeof want);
  return ok;
}
