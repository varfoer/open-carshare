#ifndef OCS_APP_H
#define OCS_APP_H

#include "auth/ratelimit.h"
#include "core/mail_build.h"
#include "store/db.h"

typedef struct {
  char listen[64], data_dir[256], web_dir[256], base_url[128];
  char smtp_url[128], smtp_user[64], smtp_pass[128], mail_from[128];
  int tracker_port; /* 0: tracker listener disabled */
  /* Read by deploy/vps/ocs-backup.sh; the server only validates them. */
  char backup_recipient[128], backup_mail_to[128], backup_dir[256];
  int64_t backup_max_mail_bytes;   /* default 20000000 */
} config_t;

/* 0 ok, -1 fail. Production wires this to mail_build + platform_mail_send. */
typedef int (*mail_send_fn)(void *ctx, const char *to, const char *subject, const char *body,
                            const mail_att_t *att);

typedef struct {
  db_t db; config_t cfg; ratelimit_t rl;
  mail_send_fn mail_send; void *mail_ctx;
} app_t;

#endif
