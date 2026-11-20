#ifndef OCS_APP_H
#define OCS_APP_H

#include "auth/ratelimit.h"
#include "core/access.h"
#include "core/mail_build.h"
#include "store/db.h"

typedef struct {
  char listen[64], data_dir[256], web_dir[256], base_url[128];
  char smtp_url[128], smtp_user[64], smtp_pass[128], mail_from[128];
  int tracker_port; /* 0: tracker listener disabled */
  /* Read by deploy/vps/ocs-backup.sh; the server only validates them. */
  char backup_recipient[128], backup_mail_to[128], backup_dir[256];
  int64_t backup_max_mail_bytes;   /* default 20000000 */
  /* igloohome key box API; urls https only, no trailing "/". Empty id/secret = not configured. */
  char igloo_client_id[128], igloo_client_secret[256], igloo_api_url[128], igloo_token_url[128];
} config_t;

/* 0 ok, -1 fail. Production wires this to mail_build + platform_mail_send. */
typedef int (*mail_send_fn)(void *ctx, const char *to, const char *subject, const char *body,
                            const mail_att_t *att);

/* Create an hourly PIN for device_id valid [from, until). Never logs. */
typedef pin_err_t (*pin_create_fn)(void *ctx, const char *device_id, int64_t from, int64_t until,
                                    int variance, const char *access_name, pin_result_t *out);
/* List the locks of the igloohome account; *http_status = last HTTP status (0 = transport). */
typedef pin_err_t (*lock_list_fn)(void *ctx, lock_info_t *out, size_t cap, size_t *n, int *http_status);

/* Variance already tried for (car, hour) in this process; see api_access. */
typedef struct { int64_t car_id, hour; int used; } pin_attempt_t;
#define APP_PIN_ATTEMPTS 16

typedef struct {
  db_t db; config_t cfg; ratelimit_t rl;
  mail_send_fn mail_send; void *mail_ctx;
  pin_create_fn pin_create; lock_list_fn lock_list; void *pin_ctx;
  pin_attempt_t pin_attempts[APP_PIN_ATTEMPTS];
} app_t;

#endif
