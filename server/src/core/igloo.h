#ifndef OCS_CORE_IGLOO_H
#define OCS_CORE_IGLOO_H

#include <stddef.h>
#include <stdint.h>
#include "core/access.h"

/* "YYYY-MM-DDTHH:00:00+hh:00" in Europe/Berlin; -1 if utc % 3600 != 0 or cap too small. */
int igloo_iso_hour(int64_t utc, char *out, size_t cap);
/* malloc'd {"variance","startDate","endDate","accessName"}; NULL on OOM/invalid. */
char *igloo_hourly_body(int64_t from, int64_t until, int variance, const char *access_name);
int igloo_parse_pin(const char *body, pin_result_t *out);                          /* 0 ok, -1 malformed */
int igloo_parse_token(const char *body, char *token, size_t cap, int64_t *expires_in);
/* Provisional shape, see builder call 8. n = items stored (at most cap). */
int igloo_parse_devices(const char *body, lock_info_t *out, size_t cap, size_t *n);

#endif
