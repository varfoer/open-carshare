#ifndef OCS_CORE_CODEC_H
#define OCS_CORE_CODEC_H

#include "cJSON.h"
#include "core/model.h"

/* X_to_json: caller owns the returned object (cJSON_Delete); NULL on OOM or if an
 * integer field has |v| > 10^15 - 1 (it would not survive printing and loading;
 * this covers the reader's 2^53 limit), never a partial object.
 * X_from_json: 0 ok; -1 on non-object, missing required field, wrong type,
 * non-integral/out-of-range number, unknown enum string or over-long string.
 * *out is zeroed first. Unknown keys are ignored. */
cJSON *user_to_json(const user_t *v);
cJSON *user_to_public_json(const user_t *v); /* no pw_hash */
cJSON *car_to_json(const car_t *v);
cJSON *booking_to_json(const booking_t *v);
cJSON *damage_report_to_json(const damage_report_t *v);
cJSON *odometer_reading_to_json(const odometer_reading_t *v);
cJSON *session_to_json(const session_t *v);
cJSON *token_to_json(const token_t *v);
cJSON *settings_to_json(const settings_t *v);
cJSON *tariff_to_json(const tariff_t *v);

int user_from_json(const cJSON *j, user_t *out);
/* tracker_imei is optional on load (absent -> ""); present must be "" or imei_valid. */
int car_from_json(const cJSON *j, car_t *out);
/* km_source "geotab" (Phase 2 data) loads as KM_TELEMATICS; it is never written.
 * Trip keys (actual_start, actual_end, km_start, km_end, km_source, km_pending) are
 * optional on load (Phase 1 lines); absent -> 0 / KM_NONE / false. */
int booking_from_json(const cJSON *j, booking_t *out);
int damage_report_from_json(const cJSON *j, damage_report_t *out); /* all keys required */
int odometer_reading_from_json(const cJSON *j, odometer_reading_t *out); /* all keys required */
int session_from_json(const cJSON *j, session_t *out);
int token_from_json(const cJSON *j, token_t *out);
/* Missing keys get defaults: start_tolerance_min 15, others 0. */
int settings_from_json(const cJSON *j, settings_t *out);

/* All keys required; -1 also if !tariff_valid. */
int tariff_from_json(const cJSON *j, tariff_t *out);
/* Money fields 0..10000000, night_start/night_end 0..1439, unused_factor_permille 0..1000. */
bool tariff_valid(const tariff_t *t);

/* True for exactly 15 ASCII digits. */
bool imei_valid(const char *s);

#endif
