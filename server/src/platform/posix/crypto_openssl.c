/* Web Push crypto primitives on OpenSSL 3 (non-deprecated EVP / EC_POINT API only). */
#include <string.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/obj_mac.h>
#include <openssl/param_build.h>

#include "platform/platform.h"

/* priv as BIGNUM if it is in [1, n-1]; NULL otherwise. */
static BIGNUM *scalar(const EC_GROUP *g, const uint8_t priv[32]) {
  BIGNUM *d = BN_bin2bn(priv, 32, NULL);
  if (!d) return NULL;
  const BIGNUM *order = EC_GROUP_get0_order(g);
  if (BN_is_zero(d) || !order || BN_cmp(d, order) >= 0) {
    BN_clear_free(d);
    return NULL;
  }
  return d;
}

int platform_p256_public(const uint8_t priv[32], uint8_t pub[65]) {
  EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
  BIGNUM *d = g ? scalar(g, priv) : NULL;
  EC_POINT *p = g ? EC_POINT_new(g) : NULL;
  int rc = -1;
  if (d && p && EC_POINT_mul(g, p, d, NULL, NULL, NULL) == 1 &&
      EC_POINT_point2oct(g, p, POINT_CONVERSION_UNCOMPRESSED, pub, 65, NULL) == 65)
    rc = 0;
  EC_POINT_free(p);
  BN_clear_free(d);
  EC_GROUP_free(g);
  return rc;
}

int platform_p256_keygen(uint8_t priv[32], uint8_t pub[65]) {
  for (int i = 0; i < 16; i++) {
    if (platform_random(priv, 32) != 0) return -1;
    if (platform_p256_public(priv, pub) == 0) return 0;
  }
  return -1;
}

/* EVP_PKEY for P-256 from a private scalar (with its public point) or a public point only. */
static EVP_PKEY *make_key(const uint8_t *priv, const uint8_t pub[65]) {
  uint8_t own[65];
  if (priv) {
    if (platform_p256_public(priv, own) != 0) return NULL;
    pub = own;
  }
  OSSL_PARAM_BLD *b = OSSL_PARAM_BLD_new();
  BIGNUM *d = priv ? BN_bin2bn(priv, 32, NULL) : NULL;
  OSSL_PARAM *params = NULL;
  EVP_PKEY *key = NULL;
  EVP_PKEY_CTX *ctx = NULL;
  if (!b || (priv && !d)) goto out;
  if (!OSSL_PARAM_BLD_push_utf8_string(b, OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) ||
      !OSSL_PARAM_BLD_push_octet_string(b, OSSL_PKEY_PARAM_PUB_KEY, pub, 65) ||
      (d && !OSSL_PARAM_BLD_push_BN(b, OSSL_PKEY_PARAM_PRIV_KEY, d)))
    goto out;
  params = OSSL_PARAM_BLD_to_param(b);
  ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
  if (!params || !ctx || EVP_PKEY_fromdata_init(ctx) != 1 ||
      EVP_PKEY_fromdata(ctx, &key, priv ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params) != 1)
    key = NULL;
out:
  EVP_PKEY_CTX_free(ctx);
  OSSL_PARAM_free(params);
  BN_clear_free(d);
  OSSL_PARAM_BLD_free(b);
  return key;
}

static int on_curve(const uint8_t pub[65]) {
  if (pub[0] != 0x04) return 0;
  EC_GROUP *g = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
  EC_POINT *p = g ? EC_POINT_new(g) : NULL;
  int ok = p && EC_POINT_oct2point(g, p, pub, 65, NULL) == 1 && EC_POINT_is_on_curve(g, p, NULL) == 1;
  EC_POINT_free(p);
  EC_GROUP_free(g);
  return ok;
}

int platform_p256_ecdh(const uint8_t priv[32], const uint8_t peer[65], uint8_t secret[32]) {
  if (!on_curve(peer)) return -1;
  EVP_PKEY *own = make_key(priv, NULL), *other = make_key(NULL, peer);
  EVP_PKEY_CTX *ctx = own ? EVP_PKEY_CTX_new_from_pkey(NULL, own, NULL) : NULL;
  size_t n = 32;
  int rc = (ctx && other && EVP_PKEY_derive_init(ctx) == 1 && EVP_PKEY_derive_set_peer(ctx, other) == 1 &&
            EVP_PKEY_derive(ctx, secret, &n) == 1 && n == 32) ? 0 : -1;
  EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(other);
  EVP_PKEY_free(own);
  return rc;
}

int platform_es256_sign(const uint8_t priv[32], const uint8_t *msg, size_t n, uint8_t sig[64]) {
  EVP_PKEY *key = make_key(priv, NULL);
  EVP_MD_CTX *md = EVP_MD_CTX_new();
  uint8_t der[80];
  size_t dlen = sizeof der;
  int rc = -1;
  if (key && md && EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, key) == 1 &&
      EVP_DigestSign(md, der, &dlen, msg, n) == 1) {
    const unsigned char *p = der;
    ECDSA_SIG *s = d2i_ECDSA_SIG(NULL, &p, (long)dlen);
    if (s && BN_bn2binpad(ECDSA_SIG_get0_r(s), sig, 32) == 32 &&
        BN_bn2binpad(ECDSA_SIG_get0_s(s), sig + 32, 32) == 32)
      rc = 0;
    ECDSA_SIG_free(s);
  }
  EVP_MD_CTX_free(md);
  EVP_PKEY_free(key);
  return rc;
}

int platform_hmac_sha256(const uint8_t *key, size_t kn, const uint8_t *msg, size_t n, uint8_t out[32]) {
  unsigned int len = 0;
  if (kn > 1024 || !HMAC(EVP_sha256(), key, (int)kn, msg, n, out, &len) || len != 32) return -1;
  return 0;
}

int platform_aes128gcm_seal(const uint8_t key[16], const uint8_t iv[12], const uint8_t *in, size_t n,
                            uint8_t *out) {
  if (n > 65536) return -1;
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  int len = 0, fin = 0, rc = -1;
  if (c && EVP_EncryptInit_ex(c, EVP_aes_128_gcm(), NULL, key, iv) == 1 &&
      EVP_EncryptUpdate(c, out, &len, in, (int)n) == 1 && EVP_EncryptFinal_ex(c, out + len, &fin) == 1 &&
      (size_t)(len + fin) == n && EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out + n) == 1)
    rc = 0;
  EVP_CIPHER_CTX_free(c);
  return rc;
}
