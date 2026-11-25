#include "platform/platform.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

static int fsync_path(const char *path, int flags) {
  int fd = open(path, flags);
  if (fd < 0) return -1;
  int rc = fsync(fd);
  if (close(fd) != 0) rc = -1;
  return rc == 0 ? 0 : -1;
}

int platform_fsync_file(const char *path) { return fsync_path(path, O_RDONLY); }
int platform_fsync_dir(const char *dir) { return fsync_path(dir, O_RDONLY | O_DIRECTORY); }

int platform_init(void) { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1; }

int64_t platform_now(void) { return (int64_t)time(NULL); }

int platform_random(void *buf, size_t n) {
  unsigned char *p = buf;
  while (n > 0) {
    ssize_t r = getrandom(p, n, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    p += r;
    n -= (size_t)r;
  }
  return 0;
}

typedef struct {
  const char *p;
  size_t left;
} mail_src_t;

static size_t mail_read_cb(char *dst, size_t size, size_t nmemb, void *ud) {
  mail_src_t *s = ud;
  size_t k = size * nmemb;
  if (k > s->left) k = s->left;
  memcpy(dst, s->p, k);
  s->p += k;
  s->left -= k;
  return k;
}

/* TLS required unless host is a local dev sink. */
static int url_is_local(const char *url) {
  const char *h = strstr(url, "://");
  h = h ? h + 3 : url;
  const char *at = strchr(h, '@');
  const char *slash = strchr(h, '/');
  if (at && (!slash || at < slash)) h = at + 1;
  static const char *const L[] = {"localhost", "127.0.0.1"};
  for (size_t i = 0; i < sizeof L / sizeof *L; i++) {
    size_t k = strlen(L[i]);
    if (strncmp(h, L[i], k) == 0 && (h[k] == '\0' || h[k] == ':' || h[k] == '/'))
      return 1;
  }
  return 0;
}

int platform_mail_send(const char *smtp_url, const char *user, const char *pass,
                       const char *from, const char *to, const char *message) {
  CURL *c = curl_easy_init();
  if (!c) return -1;
  struct curl_slist *rcpt = curl_slist_append(NULL, to);
  if (!rcpt) {
    curl_easy_cleanup(c);
    return -1;
  }
  mail_src_t src = {message, strlen(message)};
  curl_easy_setopt(c, CURLOPT_URL, smtp_url);
  curl_easy_setopt(c, CURLOPT_MAIL_FROM, from);
  curl_easy_setopt(c, CURLOPT_MAIL_RCPT, rcpt);
  curl_easy_setopt(c, CURLOPT_READFUNCTION, mail_read_cb);
  curl_easy_setopt(c, CURLOPT_READDATA, &src);
  curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_USE_SSL,
                   (long)(url_is_local(smtp_url) ? CURLUSESSL_NONE : CURLUSESSL_ALL));
  if (user && *user) curl_easy_setopt(c, CURLOPT_USERNAME, user);
  if (pass && *pass) curl_easy_setopt(c, CURLOPT_PASSWORD, pass);
  CURLcode rc = curl_easy_perform(c);
  curl_slist_free_all(rcpt);
  curl_easy_cleanup(c);
  return rc == CURLE_OK ? 0 : -1;
}

typedef struct { char *out; size_t cap, len; bool truncate; } https_sink_t;

static size_t https_write_cb(char *p, size_t sz, size_t nm, void *u) {
  https_sink_t *s = u;
  size_t n = sz * nm;
  size_t room = s->cap - 1 - s->len, k = n;
  if (k > room) {
    if (!s->truncate) return 0; /* abort: answer too large */
    k = room;
  }
  memcpy(s->out + s->len, p, k);
  s->len += k;
  return n;
}

int platform_https(const http_req_t *r, int *status, char *out, size_t cap) {
  if (!r || !r->url || !r->method || !status || !out || cap == 0 || r->timeout_s < 1) return -1;
  if (strncmp(r->url, "https://", 8) != 0) return -1;
  int post = strcmp(r->method, "POST") == 0;
  if (!post && strcmp(r->method, "GET") != 0) return -1;
  if ((r->basic_user == NULL) != (r->basic_pass == NULL)) return -1;
  CURL *c = curl_easy_init();
  if (!c) return -1;
  struct curl_slist *hdr = curl_slist_append(NULL, "Accept: application/json");
  char buf[4200];
  if (hdr && r->bearer) {
    int n = snprintf(buf, sizeof buf, "Authorization: Bearer %s", r->bearer);
    struct curl_slist *h2 = (n > 0 && (size_t)n < sizeof buf) ? curl_slist_append(hdr, buf) : NULL;
    if (!h2) { curl_slist_free_all(hdr); hdr = NULL; } else hdr = h2;
  }
  if (hdr && r->content_type) {
    int n = snprintf(buf, sizeof buf, "Content-Type: %s", r->content_type);
    struct curl_slist *h2 = (n > 0 && (size_t)n < sizeof buf) ? curl_slist_append(hdr, buf) : NULL;
    if (!h2) { curl_slist_free_all(hdr); hdr = NULL; } else hdr = h2;
  }
  for (size_t i = 0; hdr && r->headers && r->headers[i]; i++) {
    struct curl_slist *h2 = curl_slist_append(hdr, r->headers[i]);
    if (!h2) { curl_slist_free_all(hdr); hdr = NULL; } else hdr = h2;
  }
  if (!hdr) { curl_easy_cleanup(c); return -1; }
  https_sink_t sink = {out, cap, 0, r->truncate_answer};
  out[0] = 0;
  long connect_s = r->timeout_s < 5 ? r->timeout_s : 5;
  curl_easy_setopt(c, CURLOPT_URL, r->url);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, connect_s);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, r->timeout_s);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, https_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
  if (r->basic_user) {
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(c, CURLOPT_USERNAME, r->basic_user);
    curl_easy_setopt(c, CURLOPT_PASSWORD, r->basic_pass);
  }
  if (post) {
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    size_t blen = !r->body ? 0 : r->body_len ? r->body_len : strlen(r->body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)blen);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->body ? r->body : "");
  }
  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(hdr);
  curl_easy_cleanup(c);
  if (rc != CURLE_OK) { out[0] = 0; return -1; }
  out[sink.len] = 0;
  *status = (int)code;
  return 0;
}
