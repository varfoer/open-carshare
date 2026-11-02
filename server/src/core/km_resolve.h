#ifndef OCS_CORE_KM_RESOLVE_H
#define OCS_CORE_KM_RESOLVE_H

#include <stddef.h>
#include <stdint.h>

#include "core/model.h"

typedef enum { KMR_OK = 0, KMR_WAIT, KMR_BAD } kmr_result_t;

/* Derive booking km from tracker readings of car_id (order of r irrelevant).
 * KMR_WAIT: no reading after actual_end yet. KMR_BAD: readings unusable
 * (gap, odometer going backwards, km beyond 10000000). Outputs are
 * set only on KMR_OK. No allocation. */
kmr_result_t km_resolve(const odometer_reading_t *r, size_t n, int64_t car_id,
                        int64_t actual_start, int64_t actual_end,
                        int64_t *km_start, int64_t *km_end);

#endif
