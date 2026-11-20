#ifndef OCS_HTTP_IGLOO_CLIENT_H
#define OCS_HTTP_IGLOO_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "app.h"
#include "platform/platform.h"

typedef int (*igloo_http_fn)(const http_req_t *r, int *status, char *out, size_t cap);

typedef struct {
  const config_t *cfg;
  igloo_http_fn http;
  int64_t (*now)(void);
  char token[4096];
  int64_t token_exp; /* 0 = none */
} igloo_client_t;

void igloo_client_init(igloo_client_t *c, const config_t *cfg, igloo_http_fn http, int64_t (*now)(void));

/* pin_create_fn / lock_list_fn for igloohome. One 15 s budget per call, token cached and renewed
 * 300 s before expiry, one retry after a 401. Never logs; token and secret never leave the client. */
pin_err_t igloo_pin_create(void *ctx, const char *device_id, int64_t from, int64_t until,
                           int variance, const char *access_name, pin_result_t *out);
pin_err_t igloo_lock_list(void *ctx, lock_info_t *out, size_t cap, size_t *n, int *http_status);

#endif
