#include "http/webpush.h"

#include <stdio.h>
#include <string.h>

#include "core/push.h"
#include "platform/platform.h"

#define RECORD_SIZE 4096

/* HKDF-Expand to at most 32 bytes: first block T(1) = HMAC(prk, info || 0x01). */
static int expand(const uint8_t prk[32], const uint8_t *info, size_t n, uint8_t out[32]) {
  uint8_t buf[200];
  if (n + 1 > sizeof buf) return -1;
  memcpy(buf, info, n);
  buf[n] = 0x01;
  return platform_hmac_sha256(prk, 32, buf, n + 1, out);
}

int webpush_encrypt(const uint8_t ua_pub[65], const uint8_t auth[16], const uint8_t as_priv[32],
                    const uint8_t salt[16], const uint8_t *msg, size_t n, uint8_t *out, size_t cap,
                    size_t *out_len) {
  if (n > WEBPUSH_MAX_PLAIN || cap < n + WEBPUSH_OVERHEAD) return -1;
  uint8_t as_pub[65], ecdh[32], prk_key[32], ikm[32], prk[32], cek[32], nonce[32];
  if (platform_p256_public(as_priv, as_pub) != 0 || platform_p256_ecdh(as_priv, ua_pub, ecdh) != 0)
    return -1;
  /* key_info = "WebPush: info" || 0x00 || ua_public || as_public */
  uint8_t key_info[14 + 65 + 65];
  memcpy(key_info, "WebPush: info", 14); /* includes the NUL as the 0x00 separator */
  memcpy(key_info + 14, ua_pub, 65);
  memcpy(key_info + 79, as_pub, 65);
  static const char cek_info[] = "Content-Encoding: aes128gcm"; /* sizeof includes 0x00 */
  static const char nonce_info[] = "Content-Encoding: nonce";
  if (platform_hmac_sha256(auth, 16, ecdh, 32, prk_key) != 0 ||
      expand(prk_key, key_info, sizeof key_info, ikm) != 0 ||
      platform_hmac_sha256(salt, 16, ikm, 32, prk) != 0 ||
      expand(prk, (const uint8_t *)cek_info, sizeof cek_info, cek) != 0 ||
      expand(prk, (const uint8_t *)nonce_info, sizeof nonce_info, nonce) != 0)
    return -1;
  memcpy(out, salt, 16);
  out[16] = RECORD_SIZE >> 24 & 0xff;
  out[17] = RECORD_SIZE >> 16 & 0xff;
  out[18] = RECORD_SIZE >> 8 & 0xff;
  out[19] = RECORD_SIZE & 0xff;
  out[20] = 65;
  memcpy(out + 21, as_pub, 65);
  uint8_t plain[WEBPUSH_MAX_PLAIN + 1];
  memcpy(plain, msg, n);
  plain[n] = 0x02; /* last-record padding delimiter, no padding */
  if (platform_aes128gcm_seal(cek, nonce, plain, n + 1, out + 86) != 0) return -1;
  *out_len = n + WEBPUSH_OVERHEAD;
  return 0;
}

int webpush_vapid_header(const uint8_t priv[32], const uint8_t pub[65], const char *aud, const char *sub,
                         int64_t exp, char *out, size_t cap) {
  char claims[512], input[1200], part[700], sig_b64[90], pub_b64[90];
  if (strchr(aud, '"') || strchr(aud, '\\') || (sub && (strchr(sub, '"') || strchr(sub, '\\')))) return -1;
  int k = sub && *sub ? snprintf(claims, sizeof claims, "{\"aud\":\"%s\",\"exp\":%lld,\"sub\":\"%s\"}", aud,
                                 (long long)exp, sub)
                      : snprintf(claims, sizeof claims, "{\"aud\":\"%s\",\"exp\":%lld}", aud, (long long)exp);
  if (k < 0 || (size_t)k >= sizeof claims) return -1;
  static const char header[] = "{\"typ\":\"JWT\",\"alg\":\"ES256\"}";
  char hb64[64];
  if (b64url_encode((const uint8_t *)header, sizeof header - 1, hb64, sizeof hb64) != 0 ||
      b64url_encode((const uint8_t *)claims, (size_t)k, part, sizeof part) != 0)
    return -1;
  k = snprintf(input, sizeof input, "%s.%s", hb64, part);
  if (k < 0 || (size_t)k >= sizeof input) return -1;
  uint8_t sig[64];
  if (platform_es256_sign(priv, (const uint8_t *)input, (size_t)k, sig) != 0 ||
      b64url_encode(sig, 64, sig_b64, sizeof sig_b64) != 0 || b64url_encode(pub, 65, pub_b64, sizeof pub_b64) != 0)
    return -1;
  k = snprintf(out, cap, "Authorization: vapid t=%s.%s, k=%s", input, sig_b64, pub_b64);
  return k < 0 || (size_t)k >= cap ? -1 : 0;
}
