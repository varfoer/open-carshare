#ifndef OCS_CORE_PUSH_H
#define OCS_CORE_PUSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure helpers for Web Push subscriptions (spec §10): base64url and the endpoint allow-list. */

#define PUSH_MAX_PER_USER 5

/* base64url without padding; out gets a NUL. -1 if out is too small. */
int b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap);
/* base64url without padding. -1 on a character outside the alphabet, '=' or other padding,
 * an impossible length, non-zero trailing bits, or more than cap bytes. */
int b64url_decode(const char *in, uint8_t *out, size_t cap, size_t *n);

/* "https://" + host + "/" + path; host lowercase [a-z0-9.-], equal to or a subdomain of
 * fcm.googleapis.com, push.services.mozilla.com, push.apple.com or notify.windows.com; no port,
 * no userinfo; path printable ASCII without spaces; whole URL shorter than PUSH_ENDPOINT_MAX. */
bool push_endpoint_ok(const char *url);
/* "https://<host>" of an endpoint that passes push_endpoint_ok; -1 otherwise or if it does not fit. */
int push_audience(const char *url, char *out, size_t cap);

#endif
