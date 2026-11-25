#ifndef OCS_PLATFORM_PLATFORM_H
#define OCS_PLATFORM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Call once at startup, before any other platform_* call. 0 ok, -1 fail. */
int platform_init(void);

/* Durability helpers. 0 ok, -1 fail. */
int platform_fsync_file(const char *path);
int platform_fsync_dir(const char *dir);

int64_t platform_now(void);               /* time(NULL) */
int platform_random(void *buf, size_t n); /* CSPRNG fill; 0 ok, -1 fail */
/* SMTP send via libcurl. 0 ok, -1 fail. */
int platform_mail_send(const char *smtp_url, const char *user, const char *pass,
                       const char *from, const char *to, const char *message);

typedef struct {
  const char *method, *url;            /* "GET" | "POST" */
  const char *basic_user, *basic_pass; /* both NULL or both set */
  const char *bearer;                  /* NULL or token */
  const char *content_type, *body;     /* NULL = no body */
  size_t body_len;                     /* 0: strlen(body); else body is binary of this length */
  const char *const *headers;          /* NULL or NULL-terminated extra "Name: value" lines */
  long timeout_s;                      /* total; connect = min(5, timeout_s) */
} http_req_t;
/* HTTPS request via libcurl, TLS verification on. 0: answer received (*status set, out
 * NUL-terminated); -1: transport error, timeout, non-https URL or answer >= cap. */
int platform_https(const http_req_t *r, int *status, char *out, size_t cap);

/* Web Push crypto (RFC 8291/8292), OpenSSL on POSIX. P-256 private key: 32-byte big-endian
 * scalar; public key: 65-byte uncompressed point (0x04 || X || Y). All return 0 ok, -1 fail. */
int platform_p256_keygen(uint8_t priv[32], uint8_t pub[65]);
int platform_p256_public(const uint8_t priv[32], uint8_t pub[65]);  /* -1: scalar not in [1, n-1] */
/* ECDH x-coordinate; -1 if peer is not a point on the curve. */
int platform_p256_ecdh(const uint8_t priv[32], const uint8_t peer[65], uint8_t secret[32]);
/* ES256: ECDSA P-256 over SHA-256(msg), signature r || s (32 bytes each). */
int platform_es256_sign(const uint8_t priv[32], const uint8_t *msg, size_t n, uint8_t sig[64]);
int platform_hmac_sha256(const uint8_t *key, size_t kn, const uint8_t *msg, size_t n, uint8_t out[32]);
/* AES-128-GCM: out = ciphertext (n bytes) || tag (16 bytes). */
int platform_aes128gcm_seal(const uint8_t key[16], const uint8_t iv[12], const uint8_t *in, size_t n,
                            uint8_t *out);

#endif
