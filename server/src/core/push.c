#include "core/push.h"

#include <stdio.h>
#include <string.h>

#include "core/model.h"

static const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64url_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
  size_t need = n / 3 * 4 + (n % 3 ? n % 3 + 1 : 0) + 1;
  if (cap < need) return -1;
  size_t o = 0, i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
    out[o++] = B64URL[v >> 18 & 63];
    out[o++] = B64URL[v >> 12 & 63];
    out[o++] = B64URL[v >> 6 & 63];
    out[o++] = B64URL[v & 63];
  }
  if (n - i == 1) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[o++] = B64URL[v >> 18 & 63];
    out[o++] = B64URL[v >> 12 & 63];
  } else if (n - i == 2) {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;
    out[o++] = B64URL[v >> 18 & 63];
    out[o++] = B64URL[v >> 12 & 63];
    out[o++] = B64URL[v >> 6 & 63];
  }
  out[o] = '\0';
  return 0;
}

static int b64_val(char c) {
  const char *p = c ? strchr(B64URL, c) : NULL;
  return p ? (int)(p - B64URL) : -1;
}

int b64url_decode(const char *in, uint8_t *out, size_t cap, size_t *n) {
  size_t len = strlen(in);
  if (len % 4 == 1) return -1;
  size_t total = len / 4 * 3 + (len % 4 ? len % 4 - 1 : 0);
  if (total > cap) return -1;
  size_t o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < len; i++) {
    int v = b64_val(in[i]);
    if (v < 0) return -1;
    acc = acc << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (uint8_t)(acc >> bits);
      acc &= (1u << bits) - 1;
    }
  }
  if (acc != 0) return -1; /* non-canonical: leftover bits must be zero */
  *n = o;
  return 0;
}

static const char *const HOSTS[] = {"fcm.googleapis.com", "push.services.mozilla.com", "push.apple.com",
                                    "notify.windows.com"};

static bool host_allowed(const char *h, size_t n) {
  for (size_t i = 0; i < sizeof HOSTS / sizeof *HOSTS; i++) {
    size_t k = strlen(HOSTS[i]);
    if (n == k && memcmp(h, HOSTS[i], k) == 0) return true;
    if (n > k + 1 && h[n - k - 1] == '.' && memcmp(h + n - k, HOSTS[i], k) == 0) return true;
  }
  return false;
}

/* Length of the host after "https://", or 0 if the URL is not an allowed endpoint. */
static size_t endpoint_host(const char *url) {
  if (!url || strncmp(url, "https://", 8) != 0 || strlen(url) >= PUSH_ENDPOINT_MAX) return 0;
  const char *h = url + 8;
  size_t n = 0;
  while ((h[n] >= 'a' && h[n] <= 'z') || (h[n] >= '0' && h[n] <= '9') || h[n] == '-' || h[n] == '.') n++;
  if (n == 0 || h[n] != '/' || h[0] == '.' || h[n - 1] == '.' || !host_allowed(h, n)) return 0;
  for (const char *p = h + n; *p; p++)
    if (*p < 0x21 || *p > 0x7e) return 0;
  return n;
}

bool push_endpoint_ok(const char *url) { return endpoint_host(url) > 0; }

int push_audience(const char *url, char *out, size_t cap) {
  size_t n = endpoint_host(url);
  if (n == 0) return -1;
  int k = snprintf(out, cap, "https://%.*s", (int)n, url + 8);
  return k < 0 || (size_t)k >= cap ? -1 : 0;
}
