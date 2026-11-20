#ifndef OCS_CORE_ACCESS_H
#define OCS_CORE_ACCESS_H

#include <stdbool.h>
#include <stdint.h>
#include "core/model.h"

#define ACCESS_MAX_HOURS 672
#define ACCESS_MARGIN_S 900
#define ACCESS_RENEW_GAP_S 1200
#define ACCESS_VARIANCE_MAX 3

typedef enum { PIN_OK = 0, PIN_ERR_CONFIG, PIN_ERR_AUTH, PIN_ERR_PAYMENT, PIN_ERR_SCOPE,
  PIN_ERR_DEVICE, PIN_ERR_TIMEZONE, PIN_ERR_UNSUPPORTED, PIN_ERR_NETWORK, PIN_ERR_OTHER } pin_err_t;
typedef struct { char pin[16]; char pin_id[64]; int http_status; /* last status on failure, 0 = transport */ } pin_result_t;
typedef struct { char device_id[64]; char name[64]; } lock_info_t;

/* PIN validity window: from = full hour of now, until = full hour at or after
 * max(booked_end, now + 1 h), capped at from + ACCESS_MAX_HOURS. */
void access_window(int64_t now, int64_t booked_end, int64_t *from, int64_t *until);
/* pin[0] && now < valid_until - ACCESS_MARGIN_S */
bool access_usable(const access_pin_t *p, int64_t now);
/* Whether igloohome may have created a PIN for a request that ended with e and http_status:
 * success, a lost answer (PIN_ERR_NETWORK) or an answer that is neither a 4xx nor nothing sent.
 * false only when igloohome surely made none (rejected, or no request went out). */
bool access_pin_maybe_created(pin_err_t e, int http_status);
/* [A-Za-z0-9._-]{1,63}: goes into a URL path. */
bool lock_device_id_valid(const char *s);
/* English, for stderr. */
const char *pin_err_log(pin_err_t e);
/* German, for admin responses. */
const char *pin_err_admin_de(pin_err_t e);

#endif
