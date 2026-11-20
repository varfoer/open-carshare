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
  long timeout_s;                      /* total; connect = min(5, timeout_s) */
} http_req_t;
/* HTTPS request via libcurl, TLS verification on. 0: answer received (*status set, out
 * NUL-terminated); -1: transport error, timeout, non-https URL or answer >= cap. */
int platform_https(const http_req_t *r, int *status, char *out, size_t cap);

#endif
