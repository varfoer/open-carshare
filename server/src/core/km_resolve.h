#ifndef OCS_CORE_KM_RESOLVE_H
#define OCS_CORE_KM_RESOLVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/model.h"

typedef enum { KMR_OK = 0, KMR_WAIT, KMR_BAD } kmr_result_t;

/* Derive booking km from tracker readings of car_id (order of r irrelevant).
 * KMR_WAIT: no settled reading after actual_end yet. A reading with
 * ts > actual_end is settled only if its received_at <= settled_before:
 * trackers may send newest-first, so the post-return record can arrive
 * before the trip-end backlog; waiting lets the backlog land first.
 * Readings inside the window and the reading after it used for the
 * zero-km rule do not depend on settled_before. KMR_BAD: readings unusable
 * (gap, odometer going backwards, km beyond 10000000). Outputs are
 * set only on KMR_OK. No allocation. */
kmr_result_t km_resolve(const odometer_reading_t *r, size_t n, int64_t car_id,
                        int64_t actual_start, int64_t actual_end, int64_t settled_before,
                        int64_t *km_start, int64_t *km_end);

/* Provisional km of car_id from a0 to a1: last minus first reading with a0 <= ts <= a1, rounded
 * half-up to km like km_resolve. false (outputs untouched) when there is no reading in the window,
 * the odometer went backwards or a value is implausible. *as_of = ts of the last reading. */
bool km_live(const odometer_reading_t *r, size_t n, int64_t car_id, int64_t a0, int64_t a1,
             int64_t *km, int64_t *as_of);

#endif
