#include "core/codec.h"
#include "core/access.h"
#include "core/sepa.h"

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

/* Optional bool: a missing key leaves *out untouched (caller preset false); a present
 * non-bool value is an error. */
static int get_bool_opt(const cJSON *j, const char *key, bool *out) {
  if (!cJSON_GetObjectItemCaseSensitive(j, key)) return 0;
  return get_bool(j, key, out);
}

/* Optional integer: a missing key leaves *out untouched (caller preset 0). */
static int get_i64_opt(const cJSON *j, const char *key, int64_t *out) {
  if (!cJSON_GetObjectItemCaseSensitive(j, key)) return 0;
  return get_i64(j, key, out);
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

/* Optional enum: a missing key leaves *out untouched (caller preset to the first value). */
static int get_enum_opt(const cJSON *j, const char *key, const char *const *names, int count,
                        int *out) {
  if (!cJSON_GetObjectItemCaseSensitive(j, key)) return 0;
  return get_enum(j, key, names, count, out);
}

/* ---- write helpers: return 0 ok, -1 on OOM or a value the reader would reject ---- */

static int put_str(cJSON *o, const char *key, const char *s, size_t cap) {
  char buf[256];
  size_t n = strnlen(s, cap);
  if (n >= sizeof buf) n = sizeof buf - 1;
  memcpy(buf, s, n);
  buf[n] = '\0';
  return cJSON_AddStringToObject(o, key, buf) ? 0 : -1;
}

/* Largest |v| written: cJSON prints non-int32 numbers with "%1.15g" and accepts
 * that form whenever it compares equal within DBL_EPSILON, so 16-digit integers
 * (e.g. 2^53 -> 9007199254740990) would be silently altered. 15 digits always
 * print exactly. This is stricter than the reader's 2^53 limit, so every value
 * written can be loaded back unchanged. */
#define MAX_WRITE_INT 999999999999999LL /* 10^15 - 1 */

/* Refuses |v| > MAX_WRITE_INT so that nothing is written that get_i64 cannot load back. */
static int put_i64(cJSON *o, const char *key, int64_t v) {
  if (v > MAX_WRITE_INT || v < -MAX_WRITE_INT) return -1;
  return cJSON_AddNumberToObject(o, key, (double)v) ? 0 : -1;
}

static int put_cstr(cJSON *o, const char *key, const char *s) {
  return cJSON_AddStringToObject(o, key, s) ? 0 : -1;
}

static int put_bool(cJSON *o, const char *key, bool v) {
  return cJSON_AddBoolToObject(o, key, v) ? 0 : -1;
}

static const char *const ROLE_NAMES[] = {"user", "admin"};
static const char *const STATE_NAMES[] = {"reserved", "active", "done", "cancelled"};
static const char *const KIND_NAMES[] = {"reset", "invite"};
static const char *const KM_SOURCE_NAMES[] = {"none", "telematics", "manual"};
static const char *const PHASE_NAMES[] = {"start", "end"};
static const char *const MANDATE_STATUS_NAMES[] = {"active", "replaced", "revoked"};
static const char *const RUN_STATE_NAMES[] = {"draft", "final"};

static const char *enum_name(const char *const *names, int count, int v) {
  return (v >= 0 && v < count) ? names[v] : names[0];
}

#define OBJ_OR_FAIL(j) \
  do { if (!cJSON_IsObject(j)) return -1; } while (0)
#define TRY(expr) \
  do { if ((expr) != 0) return -1; } while (0)
/* In X_to_json: on any failed put, free the partial object and return NULL. */
#define PUT(expr) \
  do { if ((expr) != 0) { cJSON_Delete(o); return NULL; } } while (0)

/* ---- user ---- */

static cJSON *user_json(const user_t *v, bool with_hash) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "id", v->id));
  PUT(put_str(o, "email", v->email, sizeof v->email));
  PUT(put_str(o, "name", v->name, sizeof v->name));
  PUT(put_str(o, "street", v->street, sizeof v->street));
  PUT(put_str(o, "zip", v->zip, sizeof v->zip));
  PUT(put_str(o, "city", v->city, sizeof v->city));
  if (with_hash) PUT(put_str(o, "pw_hash", v->pw_hash, sizeof v->pw_hash));
  PUT(put_cstr(o, "role", enum_name(ROLE_NAMES, 2, v->role)));
  PUT(put_bool(o, "active", v->active));
  PUT(put_i64(o, "license_checked_at", v->license_checked_at));
  PUT(put_i64(o, "created_at", v->created_at));
  PUT(put_bool(o, "may_club_trip", v->may_club_trip));
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
  TRY(get_bool_opt(j, "may_club_trip", &out->may_club_trip));
  return 0;
}

/* ---- car ---- */

bool imei_valid(const char *s) {
  size_t n = 0;
  for (; s[n]; n++)
    if (s[n] < '0' || s[n] > '9') return false;
  return n == 15;
}

cJSON *car_to_json(const car_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "id", v->id));
  PUT(put_str(o, "name", v->name, sizeof v->name));
  PUT(put_str(o, "plate", v->plate, sizeof v->plate));
  PUT(put_str(o, "location", v->location, sizeof v->location));
  PUT(put_bool(o, "active", v->active));
  PUT(put_str(o, "tracker_imei", v->tracker_imei, sizeof v->tracker_imei));
  PUT(put_str(o, "lock_device_id", v->lock_device_id, sizeof v->lock_device_id));
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
  if (cJSON_GetObjectItemCaseSensitive(j, "tracker_imei")) {
    TRY(get_str(j, "tracker_imei", out->tracker_imei, sizeof out->tracker_imei));
    if (out->tracker_imei[0] != '\0' && !imei_valid(out->tracker_imei)) return -1;
  }
  if (cJSON_GetObjectItemCaseSensitive(j, "lock_device_id")) {
    TRY(get_str(j, "lock_device_id", out->lock_device_id, sizeof out->lock_device_id));
    if (out->lock_device_id[0] != '\0' && !lock_device_id_valid(out->lock_device_id)) return -1;
  }
  return 0;
}

/* ---- booking ---- */

cJSON *booking_to_json(const booking_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "id", v->id));
  PUT(put_i64(o, "car_id", v->car_id));
  PUT(put_i64(o, "user_id", v->user_id));
  PUT(put_i64(o, "booked_start", v->booked_start));
  PUT(put_i64(o, "booked_end", v->booked_end));
  PUT(put_cstr(o, "state", enum_name(STATE_NAMES, 4, v->state)));
  PUT(put_i64(o, "cancelled_at", v->cancelled_at));
  PUT(put_i64(o, "cancel_fee_cents", v->cancel_fee_cents));
  PUT(put_i64(o, "created_at", v->created_at));
  PUT(put_i64(o, "modified_at", v->modified_at));
  PUT(put_i64(o, "modified_by", v->modified_by));
  PUT(put_bool(o, "club_trip", v->club_trip));
  PUT(put_i64(o, "actual_start", v->actual_start));
  PUT(put_i64(o, "actual_end", v->actual_end));
  PUT(put_i64(o, "km_start", v->km_start));
  PUT(put_i64(o, "km_end", v->km_end));
  PUT(put_cstr(o, "km_source", enum_name(KM_SOURCE_NAMES, 3, v->km_source)));
  PUT(put_bool(o, "km_pending", v->km_pending));
  PUT(put_i64(o, "invoice_id", v->invoice_id));
  PUT(put_bool(o, "start_reminded", v->start_reminded));
  PUT(put_bool(o, "late_reminded", v->late_reminded));
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
  TRY(get_bool_opt(j, "club_trip", &out->club_trip));
  TRY(get_bool_opt(j, "start_reminded", &out->start_reminded));
  TRY(get_bool_opt(j, "late_reminded", &out->late_reminded));
  TRY(get_i64_opt(j, "actual_start", &out->actual_start));
  TRY(get_i64_opt(j, "actual_end", &out->actual_end));
  TRY(get_i64_opt(j, "km_start", &out->km_start));
  TRY(get_i64_opt(j, "km_end", &out->km_end));
  int km_source = KM_NONE;
  /* Phase 2 wrote "geotab"; it now means telematics. */
  const cJSON *ks = cJSON_GetObjectItemCaseSensitive(j, "km_source");
  if (cJSON_IsString(ks) && ks->valuestring && strcmp(ks->valuestring, "geotab") == 0)
    km_source = KM_TELEMATICS;
  else
    TRY(get_enum_opt(j, "km_source", KM_SOURCE_NAMES, 3, &km_source));
  out->km_source = (km_source_t)km_source;
  TRY(get_bool_opt(j, "km_pending", &out->km_pending));
  TRY(get_i64_opt(j, "invoice_id", &out->invoice_id));
  return 0;
}

/* ---- damage report ---- */

cJSON *damage_report_to_json(const damage_report_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  /* put_str truncates at 255; text is up to 1023, so it is length-checked here. */
  if (strnlen(v->text, sizeof v->text) >= sizeof v->text) PUT(-1);
  PUT(put_i64(o, "id", v->id));
  PUT(put_i64(o, "booking_id", v->booking_id));
  PUT(put_i64(o, "user_id", v->user_id));
  PUT(put_cstr(o, "phase", enum_name(PHASE_NAMES, 2, v->phase)));
  PUT(put_cstr(o, "text", v->text));
  PUT(put_i64(o, "n_photos", v->n_photos));
  PUT(put_i64(o, "created_at", v->created_at));
  return o;
}

int damage_report_from_json(const cJSON *j, damage_report_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  int phase;
  TRY(get_i64(j, "id", &out->id));
  TRY(get_i64(j, "booking_id", &out->booking_id));
  TRY(get_i64(j, "user_id", &out->user_id));
  TRY(get_enum(j, "phase", PHASE_NAMES, 2, &phase));
  out->phase = (damage_phase_t)phase;
  TRY(get_str(j, "text", out->text, sizeof out->text));
  TRY(get_int(j, "n_photos", &out->n_photos));
  /* Photo handlers index files r<id>-1..n_photos. */
  if (out->n_photos < 0 || out->n_photos > DMG_MAX_PHOTOS) return -1;
  TRY(get_i64(j, "created_at", &out->created_at));
  return 0;
}

/* ---- odometer reading ---- */

cJSON *odometer_reading_to_json(const odometer_reading_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "car_id", v->car_id));
  PUT(put_i64(o, "ts", v->ts));
  PUT(put_i64(o, "odometer_m", v->odometer_m));
  PUT(put_i64(o, "received_at", v->received_at));
  return o;
}

int odometer_reading_from_json(const cJSON *j, odometer_reading_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  TRY(get_i64(j, "car_id", &out->car_id));
  TRY(get_i64(j, "ts", &out->ts));
  TRY(get_i64(j, "odometer_m", &out->odometer_m));
  TRY(get_i64(j, "received_at", &out->received_at));
  return 0;
}

/* ---- access pin ---- */

cJSON *access_pin_to_json(const access_pin_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "id", v->id));
  PUT(put_i64(o, "booking_id", v->booking_id));
  PUT(put_i64(o, "car_id", v->car_id));
  PUT(put_str(o, "pin_id", v->pin_id, sizeof v->pin_id));
  PUT(put_str(o, "pin", v->pin, sizeof v->pin));
  PUT(put_i64(o, "valid_from", v->valid_from));
  PUT(put_i64(o, "valid_until", v->valid_until));
  PUT(put_i64(o, "variance", v->variance));
  PUT(put_i64(o, "created_at", v->created_at));
  return o;
}

int access_pin_from_json(const cJSON *j, access_pin_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  int64_t variance = 0;
  TRY(get_i64(j, "id", &out->id));
  TRY(get_i64(j, "booking_id", &out->booking_id));
  TRY(get_i64(j, "car_id", &out->car_id));
  TRY(get_str(j, "pin_id", out->pin_id, sizeof out->pin_id));
  TRY(get_str(j, "pin", out->pin, sizeof out->pin));
  TRY(get_i64(j, "valid_from", &out->valid_from));
  TRY(get_i64(j, "valid_until", &out->valid_until));
  TRY(get_i64(j, "variance", &variance));
  TRY(get_i64(j, "created_at", &out->created_at));
  if (out->booking_id < 0) return -1;
  if (variance < 1 || variance > 3) return -1;
  out->variance = (int)variance;
  size_t n = strlen(out->pin);
  if (n != 0 && (n < 4 || n > 12)) return -1;
  for (size_t i = 0; i < n; i++)
    if (out->pin[i] < '0' || out->pin[i] > '9') return -1;
  return 0;
}

/* ---- session ---- */

cJSON *session_to_json(const session_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_str(o, "token_hash", v->token_hash, sizeof v->token_hash));
  PUT(put_i64(o, "user_id", v->user_id));
  PUT(put_i64(o, "expires_at", v->expires_at));
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
  PUT(put_str(o, "token_hash", v->token_hash, sizeof v->token_hash));
  PUT(put_i64(o, "user_id", v->user_id));
  PUT(put_i64(o, "expires_at", v->expires_at));
  PUT(put_cstr(o, "kind", enum_name(KIND_NAMES, 2, v->kind)));
  PUT(put_bool(o, "used", v->used));
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
  PUT(put_i64(o, "start_tolerance_min", v->start_tolerance_min));
  PUT(put_i64(o, "cancel_deadline_min", v->cancel_deadline_min));
  PUT(put_i64(o, "cancel_fee_cents", v->cancel_fee_cents));
  PUT(put_str(o, "assoc_name", v->assoc_name, sizeof v->assoc_name));
  PUT(put_str(o, "assoc_street", v->assoc_street, sizeof v->assoc_street));
  PUT(put_str(o, "assoc_zip", v->assoc_zip, sizeof v->assoc_zip));
  PUT(put_str(o, "assoc_city", v->assoc_city, sizeof v->assoc_city));
  PUT(put_str(o, "creditor_id", v->creditor_id, sizeof v->creditor_id));
  PUT(put_str(o, "creditor_iban", v->creditor_iban, sizeof v->creditor_iban));
  PUT(put_str(o, "creditor_bic", v->creditor_bic, sizeof v->creditor_bic));
  PUT(put_i64(o, "sepa_lead_days", v->sepa_lead_days));
  PUT(put_i64(o, "reminder_before_min", v->reminder_before_min));
  /* put_str truncates at 255; the footer is up to 511, so it is length-checked here. */
  if (strnlen(v->invoice_footer, sizeof v->invoice_footer) >= sizeof v->invoice_footer) PUT(-1);
  PUT(put_cstr(o, "invoice_footer", v->invoice_footer));
  return o;
}

int settings_from_json(const cJSON *j, settings_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  out->start_tolerance_min = 15;
  out->sepa_lead_days = 5;
  out->reminder_before_min = 60;
  /* Present keys must be valid; missing keys keep their defaults. */
  if (cJSON_GetObjectItemCaseSensitive(j, "start_tolerance_min"))
    TRY(get_int(j, "start_tolerance_min", &out->start_tolerance_min));
  if (cJSON_GetObjectItemCaseSensitive(j, "cancel_deadline_min"))
    TRY(get_int(j, "cancel_deadline_min", &out->cancel_deadline_min));
  if (cJSON_GetObjectItemCaseSensitive(j, "cancel_fee_cents"))
    TRY(get_i64(j, "cancel_fee_cents", &out->cancel_fee_cents));
  if (cJSON_GetObjectItemCaseSensitive(j, "sepa_lead_days"))
    TRY(get_int(j, "sepa_lead_days", &out->sepa_lead_days));
  if (cJSON_GetObjectItemCaseSensitive(j, "reminder_before_min"))
    TRY(get_int(j, "reminder_before_min", &out->reminder_before_min));
  /* String keys default to "" when missing. */
#define SETTINGS_STR(field)                                                   \
  if (cJSON_GetObjectItemCaseSensitive(j, #field))                            \
    TRY(get_str(j, #field, out->field, sizeof out->field))
  SETTINGS_STR(assoc_name);
  SETTINGS_STR(assoc_street);
  SETTINGS_STR(assoc_zip);
  SETTINGS_STR(assoc_city);
  SETTINGS_STR(creditor_id);
  SETTINGS_STR(creditor_iban);
  SETTINGS_STR(creditor_bic);
  SETTINGS_STR(invoice_footer);
#undef SETTINGS_STR
  return 0;
}

/* ---- mandate ---- */

cJSON *mandate_to_json(const mandate_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  /* put_str truncates at 255; holder is up to 255 chars, so it is length-checked here. */
  if (strnlen(v->holder, sizeof v->holder) >= sizeof v->holder) PUT(-1);
  PUT(put_i64(o, "id", v->id));
  PUT(put_i64(o, "user_id", v->user_id));
  PUT(put_i64(o, "created_at", v->created_at));
  PUT(put_i64(o, "ended_at", v->ended_at));
  PUT(put_i64(o, "ended_by", v->ended_by));
  PUT(put_cstr(o, "status", enum_name(MANDATE_STATUS_NAMES, 3, v->status)));
  PUT(put_cstr(o, "holder", v->holder));
  PUT(put_str(o, "iban", v->iban, sizeof v->iban));
  PUT(put_str(o, "bic", v->bic, sizeof v->bic));
  PUT(put_str(o, "client_ip", v->client_ip, sizeof v->client_ip));
  PUT(put_i64(o, "text_version", v->text_version));
  PUT(put_str(o, "creditor_name", v->creditor_name, sizeof v->creditor_name));
  PUT(put_str(o, "creditor_id", v->creditor_id, sizeof v->creditor_id));
  return o;
}

int mandate_from_json(const cJSON *j, mandate_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  int status;
  TRY(get_i64(j, "id", &out->id));
  TRY(get_i64(j, "user_id", &out->user_id));
  TRY(get_i64(j, "created_at", &out->created_at));
  TRY(get_i64(j, "ended_at", &out->ended_at));
  TRY(get_i64(j, "ended_by", &out->ended_by));
  TRY(get_enum(j, "status", MANDATE_STATUS_NAMES, 3, &status));
  out->status = (mandate_status_t)status;
  TRY(get_str(j, "holder", out->holder, sizeof out->holder));
  TRY(get_str(j, "iban", out->iban, sizeof out->iban));
  TRY(get_str(j, "bic", out->bic, sizeof out->bic));
  TRY(get_str(j, "client_ip", out->client_ip, sizeof out->client_ip));
  TRY(get_int(j, "text_version", &out->text_version));
  TRY(get_str(j, "creditor_name", out->creditor_name, sizeof out->creditor_name));
  TRY(get_str(j, "creditor_id", out->creditor_id, sizeof out->creditor_id));
  if (!iban_valid(out->iban)) return -1;
  if (out->bic[0] && !bic_valid(out->bic)) return -1;
  return 0;
}

/* ---- tariff ---- */

cJSON *tariff_to_json(const tariff_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "id", v->id));
  PUT(put_str(o, "name", v->name, sizeof v->name));
  PUT(put_i64(o, "created_at", v->created_at));
  return o;
}

int tariff_from_json(const cJSON *j, tariff_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  TRY(get_i64(j, "id", &out->id));
  TRY(get_str(j, "name", out->name, sizeof out->name));
  TRY(get_i64(j, "created_at", &out->created_at));
  if (!out->name[0]) return -1;
  return 0;
}

/* ---- tariff version ---- */

#define TARIFF_MAX_CENTS 10000000

bool tariff_version_valid(const tariff_version_t *t) {
  const int64_t money[] = {t->rate_day_cents_per_h, t->rate_night_cents_per_h, t->rate_km_cents,
                           t->time_cap_cents_per_day, t->combined_cap_cents_per_day,
                           t->late_fee_cents_per_started_h};
  for (size_t i = 0; i < sizeof money / sizeof *money; i++)
    if (money[i] < 0 || money[i] > TARIFF_MAX_CENTS) return false;
  return t->night_start >= 0 && t->night_start <= 1439 && t->night_end >= 0 &&
         t->night_end <= 1439 && t->unused_factor_permille >= 0 &&
         t->unused_factor_permille <= 1000;
}

cJSON *tariff_version_to_json(const tariff_version_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "id", v->id));
  PUT(put_i64(o, "tariff_id", v->tariff_id));
  PUT(put_i64(o, "valid_from", v->valid_from));
  PUT(put_i64(o, "rate_day_cents_per_h", v->rate_day_cents_per_h));
  PUT(put_i64(o, "rate_night_cents_per_h", v->rate_night_cents_per_h));
  PUT(put_i64(o, "rate_km_cents", v->rate_km_cents));
  PUT(put_i64(o, "night_start", v->night_start));
  PUT(put_i64(o, "night_end", v->night_end));
  PUT(put_i64(o, "unused_factor_permille", v->unused_factor_permille));
  PUT(put_i64(o, "time_cap_cents_per_day", v->time_cap_cents_per_day));
  PUT(put_i64(o, "combined_cap_cents_per_day", v->combined_cap_cents_per_day));
  PUT(put_i64(o, "late_fee_cents_per_started_h", v->late_fee_cents_per_started_h));
  PUT(put_i64(o, "created_at", v->created_at));
  return o;
}

int tariff_version_from_json(const cJSON *j, tariff_version_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  TRY(get_i64(j, "id", &out->id));
  TRY(get_i64(j, "tariff_id", &out->tariff_id));
  TRY(get_i64(j, "valid_from", &out->valid_from));
  TRY(get_i64(j, "rate_day_cents_per_h", &out->rate_day_cents_per_h));
  TRY(get_i64(j, "rate_night_cents_per_h", &out->rate_night_cents_per_h));
  TRY(get_i64(j, "rate_km_cents", &out->rate_km_cents));
  TRY(get_int(j, "night_start", &out->night_start));
  TRY(get_int(j, "night_end", &out->night_end));
  TRY(get_int(j, "unused_factor_permille", &out->unused_factor_permille));
  TRY(get_i64(j, "time_cap_cents_per_day", &out->time_cap_cents_per_day));
  TRY(get_i64(j, "combined_cap_cents_per_day", &out->combined_cap_cents_per_day));
  TRY(get_i64(j, "late_fee_cents_per_started_h", &out->late_fee_cents_per_started_h));
  TRY(get_i64(j, "created_at", &out->created_at));
  if (!tariff_version_valid(out)) return -1;
  return 0;
}

/* ---- user tariff ---- */

cJSON *user_tariff_to_json(const user_tariff_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "id", v->id));
  PUT(put_i64(o, "user_id", v->user_id));
  PUT(put_i64(o, "tariff_id", v->tariff_id));
  PUT(put_i64(o, "valid_from", v->valid_from));
  PUT(put_i64(o, "created_at", v->created_at));
  PUT(put_i64(o, "created_by", v->created_by));
  return o;
}

int user_tariff_from_json(const cJSON *j, user_tariff_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  TRY(get_i64(j, "id", &out->id));
  TRY(get_i64(j, "user_id", &out->user_id));
  TRY(get_i64(j, "tariff_id", &out->tariff_id));
  TRY(get_i64(j, "valid_from", &out->valid_from));
  TRY(get_i64(j, "created_at", &out->created_at));
  TRY(get_i64(j, "created_by", &out->created_by));
  return 0;
}

/* ---- billing run ---- */

cJSON *billing_run_to_json(const billing_run_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "id", v->id));
  PUT(put_i64(o, "created_at", v->created_at));
  PUT(put_i64(o, "finalized_at", v->finalized_at));
  PUT(put_i64(o, "sent_at", v->sent_at));
  PUT(put_str(o, "period", v->period, sizeof v->period));
  PUT(put_cstr(o, "state", enum_name(RUN_STATE_NAMES, 2, v->state)));
  PUT(put_str(o, "collection_date", v->collection_date, sizeof v->collection_date));
  PUT(put_str(o, "sepa_file", v->sepa_file, sizeof v->sepa_file));
  return o;
}

int billing_run_from_json(const cJSON *j, billing_run_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  int state;
  TRY(get_i64(j, "id", &out->id));
  TRY(get_i64(j, "created_at", &out->created_at));
  TRY(get_i64(j, "finalized_at", &out->finalized_at));
  TRY(get_i64(j, "sent_at", &out->sent_at));
  TRY(get_str(j, "period", out->period, sizeof out->period));
  TRY(get_enum(j, "state", RUN_STATE_NAMES, 2, &state));
  out->state = (run_state_t)state;
  TRY(get_str(j, "collection_date", out->collection_date, sizeof out->collection_date));
  TRY(get_str(j, "sepa_file", out->sepa_file, sizeof out->sepa_file));
  return 0;
}

/* ---- invoice ---- */

cJSON *invoice_to_json(const invoice_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "number", v->number));
  PUT(put_i64(o, "user_id", v->user_id));
  PUT(put_i64(o, "run_id", v->run_id));
  PUT(put_i64(o, "total_cents", v->total_cents));
  PUT(put_i64(o, "n_lines", v->n_lines));
  PUT(put_i64(o, "created_at", v->created_at));
  PUT(put_i64(o, "issued_at", v->issued_at));
  PUT(put_i64(o, "mailed_at", v->mailed_at));
  PUT(put_i64(o, "mandate_id", v->mandate_id));
  PUT(put_str(o, "period", v->period, sizeof v->period));
  PUT(put_str(o, "name", v->name, sizeof v->name));
  PUT(put_str(o, "street", v->street, sizeof v->street));
  PUT(put_str(o, "zip", v->zip, sizeof v->zip));
  PUT(put_str(o, "city", v->city, sizeof v->city));
  return o;
}

int invoice_from_json(const cJSON *j, invoice_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  TRY(get_i64(j, "number", &out->number));
  TRY(get_i64(j, "user_id", &out->user_id));
  TRY(get_i64(j, "run_id", &out->run_id));
  TRY(get_i64(j, "total_cents", &out->total_cents));
  TRY(get_i64(j, "n_lines", &out->n_lines));
  TRY(get_i64(j, "created_at", &out->created_at));
  TRY(get_i64(j, "issued_at", &out->issued_at));
  TRY(get_i64(j, "mailed_at", &out->mailed_at));
  TRY(get_i64(j, "mandate_id", &out->mandate_id));
  TRY(get_str(j, "period", out->period, sizeof out->period));
  TRY(get_str(j, "name", out->name, sizeof out->name));
  TRY(get_str(j, "street", out->street, sizeof out->street));
  TRY(get_str(j, "zip", out->zip, sizeof out->zip));
  TRY(get_str(j, "city", out->city, sizeof out->city));
  return 0;
}

/* ---- invoice line ---- */

cJSON *invoice_line_to_json(const invoice_line_t *v) {
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  PUT(put_i64(o, "invoice_number", v->invoice_number));
  PUT(put_i64(o, "booking_id", v->booking_id));
  PUT(put_str(o, "car_name", v->car_name, sizeof v->car_name));
  PUT(put_i64(o, "start", v->start));
  PUT(put_i64(o, "end", v->end));
  PUT(put_bool(o, "never_started", v->never_started));
  PUT(put_bool(o, "cancelled", v->cancelled));
  PUT(put_bool(o, "club_trip", v->club_trip));
  PUT(put_i64(o, "day_min", v->day_min));
  PUT(put_i64(o, "night_min", v->night_min));
  PUT(put_i64(o, "unused_min", v->unused_min));
  PUT(put_i64(o, "km", v->km));
  PUT(put_i64(o, "time_cents", v->time_cents));
  PUT(put_i64(o, "km_cents", v->km_cents));
  PUT(put_i64(o, "cap_cut_cents", v->cap_cut_cents));
  PUT(put_i64(o, "late_fee_cents", v->late_fee_cents));
  PUT(put_i64(o, "cancel_fee_cents", v->cancel_fee_cents));
  PUT(put_i64(o, "total_cents", v->total_cents));
  return o;
}

int invoice_line_from_json(const cJSON *j, invoice_line_t *out) {
  OBJ_OR_FAIL(j);
  memset(out, 0, sizeof *out);
  TRY(get_i64(j, "invoice_number", &out->invoice_number));
  TRY(get_i64(j, "booking_id", &out->booking_id));
  TRY(get_str(j, "car_name", out->car_name, sizeof out->car_name));
  TRY(get_i64(j, "start", &out->start));
  TRY(get_i64(j, "end", &out->end));
  TRY(get_bool(j, "never_started", &out->never_started));
  TRY(get_bool(j, "cancelled", &out->cancelled));
  TRY(get_bool(j, "club_trip", &out->club_trip));
  TRY(get_i64(j, "day_min", &out->day_min));
  TRY(get_i64(j, "night_min", &out->night_min));
  TRY(get_i64(j, "unused_min", &out->unused_min));
  TRY(get_i64(j, "km", &out->km));
  TRY(get_i64(j, "time_cents", &out->time_cents));
  TRY(get_i64(j, "km_cents", &out->km_cents));
  TRY(get_i64(j, "cap_cut_cents", &out->cap_cut_cents));
  TRY(get_i64(j, "late_fee_cents", &out->late_fee_cents));
  TRY(get_i64(j, "cancel_fee_cents", &out->cancel_fee_cents));
  TRY(get_i64(j, "total_cents", &out->total_cents));
  return 0;
}
