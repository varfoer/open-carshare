#ifndef OCS_HTTP_WEBPUSH_H
#define OCS_HTTP_WEBPUSH_H

#include <stddef.h>
#include <stdint.h>

/* Web Push message encryption (RFC 8291, aes128gcm of RFC 8188, one record) and the VAPID
 * Authorization header (RFC 8292), on the platform crypto. */

/* Header (salt 16, rs 4, idlen 1, keyid 65) plus padding delimiter and GCM tag. */
#define WEBPUSH_OVERHEAD (86 + 1 + 16)
#define WEBPUSH_MAX_PLAIN 3000

/* Body for the browser key ua_pub/auth: as_priv and salt are the one-time key and salt (random in
 * production, fixed in tests). 0 ok, -1 on a bad ua_pub, n > WEBPUSH_MAX_PLAIN or cap too small. */
int webpush_encrypt(const uint8_t ua_pub[65], const uint8_t auth[16], const uint8_t as_priv[32],
                    const uint8_t salt[16], const uint8_t *msg, size_t n, uint8_t *out, size_t cap,
                    size_t *out_len);

/* "Authorization: vapid t=<JWT>, k=<pub>" with JWT {"aud":aud,"exp":exp[,"sub":sub]} signed ES256
 * by priv. sub NULL or "" = no sub claim. 0 ok, -1 on crypto failure or if it does not fit. */
int webpush_vapid_header(const uint8_t priv[32], const uint8_t pub[65], const char *aud, const char *sub,
                         int64_t exp, char *out, size_t cap);

#endif
