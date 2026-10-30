#ifndef OCS_PLATFORM_PLATFORM_H
#define OCS_PLATFORM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Durability helpers. 0 ok, -1 fail. */
int platform_fsync_file(const char *path);
int platform_fsync_dir(const char *dir);

int64_t platform_now(void);               /* time(NULL) */
int platform_random(void *buf, size_t n); /* CSPRNG fill; 0 ok, -1 fail */
/* SMTP send. 0 ok, -1 fail. Stub until Task 5. */
int platform_mail_send(const char *smtp_url, const char *user, const char *pass,
                       const char *from, const char *to, const char *message);

#endif
