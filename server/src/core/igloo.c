#include "core/igloo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "core/berlin.h"

int igloo_iso_hour(int64_t utc, char *out, size_t cap) {
  if (utc % 3600 != 0) return -1;
  int64_t off = berlin_offset(utc);
  time_t t = (time_t)(utc + off);
  struct tm tm;
  if (!gmtime_r(&t, &tm)) return -1;
  int oh = (int)(off / 3600);
  int n = snprintf(out, cap, "%04d-%02d-%02dT%02d:00:00+%02d:00", tm.tm_year + 1900, tm.tm_mon + 1,
                   tm.tm_mday, tm.tm_hour, oh);
  if (n < 0 || (size_t)n >= cap) return -1;
  return 0;
}

char *igloo_hourly_body(int64_t from, int64_t until, int variance, const char *access_name) {
  char a[32], b[32];
  if (variance < 1 || variance > ACCESS_VARIANCE_MAX || !access_name) return NULL;
  if (igloo_iso_hour(from, a, sizeof a) || igloo_iso_hour(until, b, sizeof b)) return NULL;
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  char *s = NULL;
  if (cJSON_AddNumberToObject(o, "variance", variance) && cJSON_AddStringToObject(o, "startDate", a) &&
      cJSON_AddStringToObject(o, "endDate", b) && cJSON_AddStringToObject(o, "accessName", access_name))
    s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  return s;
}

static const char *str_of(const cJSON *j, const char *key) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsString(it) && it->valuestring ? it->valuestring : NULL;
}

int igloo_parse_pin(const char *body, pin_result_t *out) {
  cJSON *j = cJSON_Parse(body);
  if (!j) return -1;
  int rc = -1;
  const char *pin = str_of(j, "pin"), *id = str_of(j, "pinId");
  if (pin && id) {
    size_t n = strlen(pin), m = strlen(id);
    bool digits = true;
    for (size_t i = 0; i < n; i++) if (pin[i] < '0' || pin[i] > '9') digits = false;
    bool printable = true;
    for (size_t i = 0; i < m; i++) if ((unsigned char)id[i] < 0x20 || (unsigned char)id[i] >= 0x7f) printable = false;
    if (digits && printable && n >= 4 && n <= 12 && m >= 1 && m < sizeof out->pin_id) {
      memset(out, 0, sizeof *out);
      memcpy(out->pin, pin, n);
      memcpy(out->pin_id, id, m);
      rc = 0;
    }
  }
  cJSON_Delete(j);
  return rc;
}

int igloo_parse_token(const char *body, char *token, size_t cap, int64_t *expires_in) {
  cJSON *j = cJSON_Parse(body);
  if (!j) return -1;
  int rc = -1;
  const char *t = str_of(j, "access_token");
  const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "expires_in");
  if (t && t[0] && strlen(t) < cap) {
    memcpy(token, t, strlen(t) + 1);
    double v = cJSON_IsNumber(e) ? e->valuedouble : 0;
    if (!isfinite(v) || v < 0) v = 0;
    if (v > 604800) v = 604800; /* one week */
    *expires_in = (int64_t)v;
    rc = 0;
  }
  cJSON_Delete(j);
  return rc;
}

int igloo_parse_devices(const char *body, lock_info_t *out, size_t cap, size_t *n) {
  cJSON *j = cJSON_Parse(body);
  if (!j) return -1;
  const cJSON *arr = j;
  if (cJSON_IsObject(j)) {
    static const char *keys[] = {"payload", "devices", "data"};
    arr = NULL;
    for (size_t k = 0; k < 3 && !arr; k++) {
      const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, keys[k]);
      if (cJSON_IsArray(it)) arr = it;
    }
  }
  if (!cJSON_IsArray(arr)) { cJSON_Delete(j); return -1; }
  size_t cnt = 0;
  const cJSON *it;
  cJSON_ArrayForEach(it, arr) {
    if (cnt >= cap) break;
    if (!cJSON_IsObject(it)) continue;
    const char *id = str_of(it, "deviceId");
    if (!id) id = str_of(it, "id");
    if (!id || !lock_device_id_valid(id)) continue;
    const char *nm = str_of(it, "deviceName");
    if (!nm) nm = str_of(it, "name");
    if (!nm) nm = "";
    if (strlen(nm) >= sizeof out[cnt].name) nm = "";
    memset(&out[cnt], 0, sizeof out[cnt]);
    memcpy(out[cnt].device_id, id, strlen(id) + 1);
    memcpy(out[cnt].name, nm, strlen(nm) + 1);
    cnt++;
  }
  *n = cnt;
  cJSON_Delete(j);
  return 0;
}
