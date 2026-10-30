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
cJSON *session_to_json(const session_t *v);
cJSON *token_to_json(const token_t *v);
cJSON *settings_to_json(const settings_t *v);

int user_from_json(const cJSON *j, user_t *out);
int car_from_json(const cJSON *j, car_t *out);
int booking_from_json(const cJSON *j, booking_t *out);
int session_from_json(const cJSON *j, session_t *out);
int token_from_json(const cJSON *j, token_t *out);
/* Missing keys get defaults: start_tolerance_min 15, others 0. */
int settings_from_json(const cJSON *j, settings_t *out);

#endif
