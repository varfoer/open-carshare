#include "core/codec.h"

#include <string.h>

#define MAX_SAFE_INT 9007199254740992LL /* 2^53 */

/* ---- read helpers: return 0 ok, -1 on missing/wrong type/range ---- */

static int get_i64(const cJSON *j, const char *key, int64_t *out) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
  if (!cJSON_IsNumber(it)) return -1;
  /* valuedouble is read only to check integrality and range. */
  if (it->valuedouble > (double)MAX_SAFE_INT || it->valuedouble < -(double)MAX_SAFE_INT) return -1;
  int64_t v = (int64_t)it->valuedouble;
  if ((double)v != it->valuedouble) return -1;
  *out = v;
  return 0;
}

static int get_int(const cJSON *j, const char *key, int *out) {
  int64_t v;
  if (get_i64(j, key, &v) != 0) return -1;
  if (v < INT32_MIN || v > INT32_MAX) return -1;
  *out = (int)v;
  return 0;
}

static int get_bool(const cJSON *j, const char *key, bool *out) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
  if (!cJSON_IsBool(it)) return -1;
  *out = cJSON_IsTrue(it);
  return 0;
}

/* Copies into a fixed buffer of size cap; rejects strings that do not fit with NUL. */
static int get_str(const cJSON *j, const char *key, char *dst, size_t cap) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
  if (!cJSON_IsString(it) || it->valuestring == NULL) return -1;
  size_t n = strlen(it->valuestring);
  if (n >= cap) return -1;
  memcpy(dst, it->valuestring, n);
  dst[n] = '\0';
  return 0;
}

static int get_enum(const cJSON *j, const char *key, const char *const *names, int count, int *out) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(j, key);
  if (!cJSON_IsString(it) || it->valuestring == NULL) return -1;
  for (int i = 0; i < count; i++) {
    if (strcmp(it->valuestring, names[i]) == 0) {
      *out = i;
      return 0;
    }
  }
  return -1;
}

/* ---- write helpers ---- */

static void put_str(cJSON *o, const char *key, const char *s, size_t cap) {
  char buf[256];
  size_t n = strnlen(s, cap);
  if (n >= sizeof buf) n = sizeof buf - 1;
  memcpy(buf, s, n);
  buf[n] = '\0';
  cJSON_AddStringToObject(o, key, buf);
}

static void put_i64(cJSON *o, const char *key, int64_t v) {
  cJSON_AddNumberToObject(o, key, (double)v);
}

static const char *const ROLE_NAMES[] = {"user", "admin"};
static const char *const STATE_NAMES[] = {"reserved", "active", "done", "cancelled"};
static const char *const KIND_NAMES[] = {"reset", "invite"};

static const char *enum_name(const char *const *names, int count, int v) {
  return (v >= 0 && v < count) ? names[v] : names[0];
}

#define OBJ_OR_FAIL(j) \
  do { if (!cJSON_IsObject(j)) return -1; } while (0)
#define TRY(expr) \
  do { if ((expr) != 0) return -1; } while (0)

/* ---- user ---- */

static cJSON *user_json(const user_t *v, bool with_hash) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  put_i64(o, "id", v->id);
  put_str(o, "email", v->email, sizeof v->email);
  put_str(o, "name", v->name, sizeof v->name);
  put_str(o, "street", v->street, sizeof v->street);
  put_str(o, "zip", v->zip, sizeof v->zip);
  put_str(o, "city", v->city, sizeof v->city);
  if (with_hash) put_str(o, "pw_hash", v->pw_hash, sizeof v->pw_hash);
  cJSON_AddStringToObject(o, "role", enum_name(ROLE_NAMES, 2, v->role));
  cJSON_AddBoolToObject(o, "active", v->active);
  put_i64(o, "license_checked_at", v->license_checked_at);
  put_i64(o, "created_at", v->created_at);
  return o;
}

cJSON *user_to_json(const user_t *v) { return user_json(v, true); }
cJSON *user_to_public_json(const user_t *v) { return user_json(v, false); }

int user_from_json(const cJSON *j, user_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  int role;
  TRY(get_i64(j, "id", &out->id));
  TRY(get_str(j, "email", out->email, sizeof out->email));
  TRY(get_str(j, "name", out->name, sizeof out->name));
  TRY(get_str(j, "street", out->street, sizeof out->street));
  TRY(get_str(j, "zip", out->zip, sizeof out->zip));
  TRY(get_str(j, "city", out->city, sizeof out->city));
  TRY(get_str(j, "pw_hash", out->pw_hash, sizeof out->pw_hash));
  TRY(get_enum(j, "role", ROLE_NAMES, 2, &role));
  out->role = (role_t)role;
  TRY(get_bool(j, "active", &out->active));
  TRY(get_i64(j, "license_checked_at", &out->license_checked_at));
  TRY(get_i64(j, "created_at", &out->created_at));
  return 0;
}

/* ---- car ---- */

cJSON *car_to_json(const car_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  put_i64(o, "id", v->id);
  put_str(o, "name", v->name, sizeof v->name);
  put_str(o, "plate", v->plate, sizeof v->plate);
  put_str(o, "location", v->location, sizeof v->location);
  cJSON_AddBoolToObject(o, "active", v->active);
  return o;
}

int car_from_json(const cJSON *j, car_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  TRY(get_i64(j, "id", &out->id));
  TRY(get_str(j, "name", out->name, sizeof out->name));
  TRY(get_str(j, "plate", out->plate, sizeof out->plate));
  TRY(get_str(j, "location", out->location, sizeof out->location));
  TRY(get_bool(j, "active", &out->active));
  return 0;
}

/* ---- booking ---- */

cJSON *booking_to_json(const booking_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  put_i64(o, "id", v->id);
  put_i64(o, "car_id", v->car_id);
  put_i64(o, "user_id", v->user_id);
  put_i64(o, "booked_start", v->booked_start);
  put_i64(o, "booked_end", v->booked_end);
  cJSON_AddStringToObject(o, "state", enum_name(STATE_NAMES, 4, v->state));
  put_i64(o, "cancelled_at", v->cancelled_at);
  put_i64(o, "cancel_fee_cents", v->cancel_fee_cents);
  put_i64(o, "created_at", v->created_at);
  put_i64(o, "modified_at", v->modified_at);
  put_i64(o, "modified_by", v->modified_by);
  return o;
}

int booking_from_json(const cJSON *j, booking_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  int state;
  TRY(get_i64(j, "id", &out->id));
  TRY(get_i64(j, "car_id", &out->car_id));
  TRY(get_i64(j, "user_id", &out->user_id));
  TRY(get_i64(j, "booked_start", &out->booked_start));
  TRY(get_i64(j, "booked_end", &out->booked_end));
  TRY(get_enum(j, "state", STATE_NAMES, 4, &state));
  out->state = (booking_state_t)state;
  TRY(get_i64(j, "cancelled_at", &out->cancelled_at));
  TRY(get_i64(j, "cancel_fee_cents", &out->cancel_fee_cents));
  TRY(get_i64(j, "created_at", &out->created_at));
  TRY(get_i64(j, "modified_at", &out->modified_at));
  TRY(get_i64(j, "modified_by", &out->modified_by));
  return 0;
}

/* ---- session ---- */

cJSON *session_to_json(const session_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  put_str(o, "token_hash", v->token_hash, sizeof v->token_hash);
  put_i64(o, "user_id", v->user_id);
  put_i64(o, "expires_at", v->expires_at);
  return o;
}

int session_from_json(const cJSON *j, session_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  TRY(get_str(j, "token_hash", out->token_hash, sizeof out->token_hash));
  TRY(get_i64(j, "user_id", &out->user_id));
  TRY(get_i64(j, "expires_at", &out->expires_at));
  return 0;
}

/* ---- token ---- */

cJSON *token_to_json(const token_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  put_str(o, "token_hash", v->token_hash, sizeof v->token_hash);
  put_i64(o, "user_id", v->user_id);
  put_i64(o, "expires_at", v->expires_at);
  cJSON_AddStringToObject(o, "kind", enum_name(KIND_NAMES, 2, v->kind));
  cJSON_AddBoolToObject(o, "used", v->used);
  return o;
}

int token_from_json(const cJSON *j, token_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  int kind;
  TRY(get_str(j, "token_hash", out->token_hash, sizeof out->token_hash));
  TRY(get_i64(j, "user_id", &out->user_id));
  TRY(get_i64(j, "expires_at", &out->expires_at));
  TRY(get_enum(j, "kind", KIND_NAMES, 2, &kind));
  out->kind = (token_kind_t)kind;
  TRY(get_bool(j, "used", &out->used));
  return 0;
}

/* ---- settings ---- */

cJSON *settings_to_json(const settings_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  put_i64(o, "start_tolerance_min", v->start_tolerance_min);
  put_i64(o, "cancel_deadline_min", v->cancel_deadline_min);
  put_i64(o, "cancel_fee_cents", v->cancel_fee_cents);
  return o;
}

int settings_from_json(const cJSON *j, settings_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  out->start_tolerance_min = 15;
  /* Present keys must be valid; missing keys keep their defaults. */
  if (cJSON_GetObjectItemCaseSensitive(j, "start_tolerance_min"))
    TRY(get_int(j, "start_tolerance_min", &out->start_tolerance_min));
  if (cJSON_GetObjectItemCaseSensitive(j, "cancel_deadline_min"))
    TRY(get_int(j, "cancel_deadline_min", &out->cancel_deadline_min));
  if (cJSON_GetObjectItemCaseSensitive(j, "cancel_fee_cents"))
    TRY(get_i64(j, "cancel_fee_cents", &out->cancel_fee_cents));
  return 0;
}
