#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include "unity.h"
#include "cJSON.h"
#include "core/model.h"
#include "core/push.h"
#include "http/webpush.h"
#include "platform/platform.h"

void setUp(void) {}
void tearDown(void) {}

/* RFC 8291 section 5 / appendix A. */
#define AS_PRIV "yfWPiYE-n46HLnH0KqZOF1fJJU3MYrct3AELtAQ-oRw"
#define AS_PUB "BP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8"
#define UA_PUB "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4"
#define SALT "DGv6ra1nlYgDCS1FRnbzlw"
#define AUTH "BTBZMqHH6r4Tts7J_aSIgg"
#define ECDH "kyrL1jIIOHEzg3sM2ZWRHDRB62YACZhhSlknJ672kSs"
#define HEADER "DGv6ra1nlYgDCS1FRnbzlwAAEABBBP4z9KsN6nGRTbVYI_c7VJSPQTBtkgcy27mlmlMoZIIgDll6e3vCYLocInmYWAmS6TlzAC8wEqKK6PBru3jl7A8"
#define CIPHER "8pfeW0KbunFT06SuDKoJH9Ql87S1QUrdirN6GcG7sFz1y1sqLgVi1VhjVkHsUoEsbI_0LpXMuGvnzQ"
#define PLAIN "When I grow up, I want to be a watermelon"

static size_t dec(const char *s, uint8_t *out, size_t cap) {
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT(0, b64url_decode(s, out, cap, &n));
  return n;
}

static void test_b64url(void) {
  char s[16];
  uint8_t b[16];
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT(0, b64url_encode((const uint8_t *)"\xfb\xff", 2, s, sizeof s));
  TEST_ASSERT_EQUAL_STRING("-_8", s);
  TEST_ASSERT_EQUAL_INT(0, b64url_decode("-_8", b, sizeof b, &n));
  TEST_ASSERT_EQUAL_size_t(2, n);
  TEST_ASSERT_EQUAL_HEX8(0xfb, b[0]);
  TEST_ASSERT_EQUAL_INT(0, b64url_encode((const uint8_t *)"abc", 3, s, sizeof s));
  TEST_ASSERT_EQUAL_STRING("YWJj", s);
  TEST_ASSERT_EQUAL_INT(0, b64url_encode((const uint8_t *)"a", 1, s, sizeof s));
  TEST_ASSERT_EQUAL_STRING("YQ", s);
  TEST_ASSERT_EQUAL_INT(-1, b64url_encode((const uint8_t *)"abc", 3, s, 4)); /* needs 5 */
  TEST_ASSERT_EQUAL_INT(-1, b64url_decode("YQ==", b, sizeof b, &n));  /* padding */
  TEST_ASSERT_EQUAL_INT(-1, b64url_decode("Y+Q", b, sizeof b, &n));   /* standard alphabet */
  TEST_ASSERT_EQUAL_INT(-1, b64url_decode("Y", b, sizeof b, &n));     /* impossible length */
  TEST_ASSERT_EQUAL_INT(-1, b64url_decode("YR", b, sizeof b, &n));    /* trailing bits set */
  TEST_ASSERT_EQUAL_INT(-1, b64url_decode("YWJj", b, 2, &n));         /* over cap */
  TEST_ASSERT_EQUAL_INT(0, b64url_decode("", b, sizeof b, &n));
  TEST_ASSERT_EQUAL_size_t(0, n);
}

static void test_rfc8291_vector(void) {
  uint8_t as_priv[32], as_pub[65], ua_pub[65], salt[16], auth[16], ecdh[32], want_ecdh[32], pub[65];
  TEST_ASSERT_EQUAL_size_t(32, dec(AS_PRIV, as_priv, sizeof as_priv));
  TEST_ASSERT_EQUAL_size_t(65, dec(AS_PUB, as_pub, sizeof as_pub));
  TEST_ASSERT_EQUAL_size_t(65, dec(UA_PUB, ua_pub, sizeof ua_pub));
  TEST_ASSERT_EQUAL_size_t(16, dec(SALT, salt, sizeof salt));
  TEST_ASSERT_EQUAL_size_t(16, dec(AUTH, auth, sizeof auth));
  TEST_ASSERT_EQUAL_size_t(32, dec(ECDH, want_ecdh, sizeof want_ecdh));
  TEST_ASSERT_EQUAL_INT(0, platform_p256_public(as_priv, pub));
  TEST_ASSERT_EQUAL_MEMORY(as_pub, pub, 65);
  TEST_ASSERT_EQUAL_INT(0, platform_p256_ecdh(as_priv, ua_pub, ecdh));
  TEST_ASSERT_EQUAL_MEMORY(want_ecdh, ecdh, 32);

  uint8_t want[256], out[256];
  size_t hn = dec(HEADER, want, sizeof want);
  TEST_ASSERT_EQUAL_size_t(86, hn);
  size_t cn = dec(CIPHER, want + hn, sizeof want - hn);
  size_t n = 0;
  TEST_ASSERT_EQUAL_INT(0, webpush_encrypt(ua_pub, auth, as_priv, salt, (const uint8_t *)PLAIN, strlen(PLAIN),
                                           out, sizeof out, &n));
  TEST_ASSERT_EQUAL_size_t(hn + cn, n);
  TEST_ASSERT_EQUAL_size_t(strlen(PLAIN) + WEBPUSH_OVERHEAD, n);
  TEST_ASSERT_EQUAL_MEMORY(want, out, n);

  /* cap too small, plaintext too long, browser key off the curve */
  TEST_ASSERT_EQUAL_INT(-1, webpush_encrypt(ua_pub, auth, as_priv, salt, (const uint8_t *)PLAIN, strlen(PLAIN),
                                            out, n - 1, &n));
  static uint8_t big[WEBPUSH_MAX_PLAIN + 1 + WEBPUSH_OVERHEAD];
  TEST_ASSERT_EQUAL_INT(-1, webpush_encrypt(ua_pub, auth, as_priv, salt, big, WEBPUSH_MAX_PLAIN + 1, big,
                                            sizeof big, &n));
  ua_pub[64] ^= 1;
  TEST_ASSERT_EQUAL_INT(-1, webpush_encrypt(ua_pub, auth, as_priv, salt, (const uint8_t *)PLAIN, strlen(PLAIN),
                                            out, sizeof out, &n));
}

static void test_keygen_ecdh(void) {
  uint8_t a[32], ap[65], b[32], bp[65], s1[32], s2[32], pub[65];
  TEST_ASSERT_EQUAL_INT(0, platform_p256_keygen(a, ap));
  TEST_ASSERT_EQUAL_INT(0, platform_p256_keygen(b, bp));
  TEST_ASSERT_EQUAL_HEX8(0x04, ap[0]);
  TEST_ASSERT_EQUAL_INT(0, platform_p256_ecdh(a, bp, s1));
  TEST_ASSERT_EQUAL_INT(0, platform_p256_ecdh(b, ap, s2));
  TEST_ASSERT_EQUAL_MEMORY(s1, s2, 32);
  uint8_t zero[32] = {0}, ff[32];
  memset(ff, 0xff, sizeof ff);
  TEST_ASSERT_EQUAL_INT(-1, platform_p256_public(zero, pub));
  TEST_ASSERT_EQUAL_INT(-1, platform_p256_public(ff, pub)); /* >= group order */
}

/* Verifies an ES256 r||s signature with OpenSSL directly. */
static int verify(const uint8_t pub[65], const char *msg, size_t n, const uint8_t sig[64]) {
  OSSL_PARAM_BLD *b = OSSL_PARAM_BLD_new();
  OSSL_PARAM_BLD_push_utf8_string(b, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
  OSSL_PARAM_BLD_push_octet_string(b, OSSL_PKEY_PARAM_PUB_KEY, pub, 65);
  OSSL_PARAM *p = OSSL_PARAM_BLD_to_param(b);
  EVP_PKEY_CTX *kc = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
  EVP_PKEY *key = NULL;
  EVP_PKEY_fromdata_init(kc);
  EVP_PKEY_fromdata(kc, &key, EVP_PKEY_PUBLIC_KEY, p);
  ECDSA_SIG *s = ECDSA_SIG_new();
  ECDSA_SIG_set0(s, BN_bin2bn(sig, 32, NULL), BN_bin2bn(sig + 32, 32, NULL));
  unsigned char *der = NULL;
  int dlen = i2d_ECDSA_SIG(s, &der);
  EVP_MD_CTX *md = EVP_MD_CTX_new();
  int ok = EVP_DigestVerifyInit(md, NULL, EVP_sha256(), NULL, key) == 1 &&
           EVP_DigestVerify(md, der, (size_t)dlen, (const unsigned char *)msg, n) == 1;
  EVP_MD_CTX_free(md);
  OPENSSL_free(der);
  ECDSA_SIG_free(s);
  EVP_PKEY_free(key);
  EVP_PKEY_CTX_free(kc);
  OSSL_PARAM_free(p);
  OSSL_PARAM_BLD_free(b);
  return ok;
}

static cJSON *jwt_part(const char *s, size_t n) {
  char tmp[600];
  uint8_t raw[600];
  size_t rn = 0;
  memcpy(tmp, s, n);
  tmp[n] = 0;
  TEST_ASSERT_EQUAL_INT(0, b64url_decode(tmp, raw, sizeof raw - 1, &rn));
  raw[rn] = 0;
  return cJSON_Parse((const char *)raw);
}

static void test_vapid_header(void) {
  uint8_t priv[32], pub[65];
  TEST_ASSERT_EQUAL_INT(0, platform_p256_keygen(priv, pub));
  char h[1200];
  TEST_ASSERT_EQUAL_INT(0, webpush_vapid_header(priv, pub, "https://fcm.googleapis.com",
                                                "https://carshare.test", 1795179600, h, sizeof h));
  const char *pre = "Authorization: vapid t=";
  TEST_ASSERT_EQUAL_INT(0, strncmp(h, pre, strlen(pre)));
  const char *t = h + strlen(pre);
  const char *d1 = strchr(t, '.'), *d2 = d1 ? strchr(d1 + 1, '.') : NULL, *k = strstr(t, ", k=");
  TEST_ASSERT_NOT_NULL(d2);
  TEST_ASSERT_NOT_NULL(k);
  cJSON *hdr = jwt_part(t, (size_t)(d1 - t));
  TEST_ASSERT_EQUAL_STRING("ES256", cJSON_GetObjectItem(hdr, "alg")->valuestring);
  TEST_ASSERT_EQUAL_STRING("JWT", cJSON_GetObjectItem(hdr, "typ")->valuestring);
  cJSON_Delete(hdr);
  cJSON *cl = jwt_part(d1 + 1, (size_t)(d2 - d1 - 1));
  TEST_ASSERT_EQUAL_STRING("https://fcm.googleapis.com", cJSON_GetObjectItem(cl, "aud")->valuestring);
  TEST_ASSERT_EQUAL_STRING("https://carshare.test", cJSON_GetObjectItem(cl, "sub")->valuestring);
  TEST_ASSERT_TRUE(cJSON_GetObjectItem(cl, "exp")->valuedouble == 1795179600.0);
  cJSON_Delete(cl);
  char sb[100];
  memcpy(sb, d2 + 1, (size_t)(k - d2 - 1));
  sb[k - d2 - 1] = 0;
  uint8_t sig[64], kpub[65];
  TEST_ASSERT_EQUAL_size_t(64, dec(sb, sig, sizeof sig));
  TEST_ASSERT_EQUAL_size_t(65, dec(k + 4, kpub, sizeof kpub));
  TEST_ASSERT_EQUAL_MEMORY(pub, kpub, 65);
  TEST_ASSERT_TRUE(verify(pub, t, (size_t)(d2 - t), sig));

  /* without sub; quotes rejected; too small */
  TEST_ASSERT_EQUAL_INT(0, webpush_vapid_header(priv, pub, "https://a.push.apple.com", NULL, 1, h, sizeof h));
  t = h + strlen(pre);
  d1 = strchr(t, '.');
  d2 = strchr(d1 + 1, '.');
  cl = jwt_part(d1 + 1, (size_t)(d2 - d1 - 1));
  TEST_ASSERT_NULL(cJSON_GetObjectItem(cl, "sub"));
  cJSON_Delete(cl);
  TEST_ASSERT_EQUAL_INT(-1, webpush_vapid_header(priv, pub, "https://x\"", NULL, 1, h, sizeof h));
  TEST_ASSERT_EQUAL_INT(-1, webpush_vapid_header(priv, pub, "https://a.push.apple.com", NULL, 1, h, 100));
}

static void test_endpoint_allow_list(void) {
  TEST_ASSERT_TRUE(push_endpoint_ok("https://fcm.googleapis.com/fcm/send/abc:DEF-_1"));
  TEST_ASSERT_TRUE(push_endpoint_ok("https://updates.push.services.mozilla.com/wpush/v2/gAAAA"));
  TEST_ASSERT_TRUE(push_endpoint_ok("https://web.push.apple.com/QGuQ"));
  TEST_ASSERT_TRUE(push_endpoint_ok("https://wns2-par02p.notify.windows.com/w/?token=BQYAAA%3d"));
  TEST_ASSERT_FALSE(push_endpoint_ok(NULL));
  TEST_ASSERT_FALSE(push_endpoint_ok("http://fcm.googleapis.com/fcm/send/abc"));
  TEST_ASSERT_FALSE(push_endpoint_ok("https://fcm.googleapis.com"));                /* no path */
  TEST_ASSERT_FALSE(push_endpoint_ok("https://fcm.googleapis.com:8443/x"));         /* port */
  TEST_ASSERT_FALSE(push_endpoint_ok("https://u@fcm.googleapis.com/x"));            /* userinfo */
  TEST_ASSERT_FALSE(push_endpoint_ok("https://evilfcm.googleapis.com.example/x"));
  TEST_ASSERT_FALSE(push_endpoint_ok("https://notfcm.googleapis.com/x"));
  TEST_ASSERT_FALSE(push_endpoint_ok("https://fcm.googleapis.com.evil.org/x"));
  TEST_ASSERT_FALSE(push_endpoint_ok("https://FCM.googleapis.com/x"));              /* uppercase */
  TEST_ASSERT_FALSE(push_endpoint_ok("https://.push.apple.com/x"));
  TEST_ASSERT_FALSE(push_endpoint_ok("https://10.0.0.1/x"));
  TEST_ASSERT_FALSE(push_endpoint_ok("https://web.push.apple.com/a b"));
  char long_url[PUSH_ENDPOINT_MAX + 8];
  strcpy(long_url, "https://web.push.apple.com/");
  size_t n = strlen(long_url);
  memset(long_url + n, 'a', PUSH_ENDPOINT_MAX - 1 - n);
  long_url[PUSH_ENDPOINT_MAX - 1] = 0; /* 1023 bytes */
  TEST_ASSERT_TRUE(push_endpoint_ok(long_url));
  long_url[PUSH_ENDPOINT_MAX - 1] = 'a';
  long_url[PUSH_ENDPOINT_MAX] = 0; /* 1024 bytes */
  TEST_ASSERT_FALSE(push_endpoint_ok(long_url));

  char aud[64];
  TEST_ASSERT_EQUAL_INT(0, push_audience("https://web.push.apple.com/QGuQ", aud, sizeof aud));
  TEST_ASSERT_EQUAL_STRING("https://web.push.apple.com", aud);
  TEST_ASSERT_EQUAL_INT(-1, push_audience("https://evil.org/x", aud, sizeof aud));
  TEST_ASSERT_EQUAL_INT(-1, push_audience("https://web.push.apple.com/QGuQ", aud, 10));
}

int main(void) {
  if (platform_init() != 0) return 1;
  UNITY_BEGIN();
  RUN_TEST(test_b64url);
  RUN_TEST(test_rfc8291_vector);
  RUN_TEST(test_keygen_ecdh);
  RUN_TEST(test_vapid_header);
  RUN_TEST(test_endpoint_allow_list);
  return UNITY_END();
}
