#include "core/km_resolve.h"

#include <stdbool.h>

#define KM_MAX INT64_C(10000000)
#define M_MAX INT64_C(10000000000000) /* keeps the rounding arithmetic far from overflow */

kmr_result_t km_resolve(const odometer_reading_t *r, size_t n, int64_t car_id,
                        int64_t actual_start, int64_t actual_end, int64_t settled_before,
                        int64_t *km_start, int64_t *km_end) {
  const odometer_reading_t *lo = NULL, *hi = NULL, *before = NULL, *after = NULL;
  bool settled = false;
  for (size_t i = 0; i < n; i++) {
    const odometer_reading_t *x = &r[i];
    if (x->car_id != car_id) continue;
    if (x->ts < actual_start) {
      if (!before || x->ts > before->ts) before = x;
    } else if (x->ts <= actual_end) {
      if (!lo || x->ts < lo->ts) lo = x;
      if (!hi || x->ts > hi->ts) hi = x;
    } else {
      if (!after || x->ts < after->ts) after = x;
      if (x->received_at <= settled_before) settled = true;
    }
  }
  if (!after || !settled) return KMR_WAIT;
  int64_t start_m, end_m;
  if (lo) {
    start_m = lo->odometer_m;
    end_m = hi->odometer_m;
  } else {
    if (!before || before->odometer_m != after->odometer_m) return KMR_BAD;
    start_m = end_m = after->odometer_m;
  }
  if (start_m < 0 || start_m > M_MAX || end_m < 0 || end_m > M_MAX || end_m < start_m)
    return KMR_BAD;
  int64_t ks = (start_m + 500) / 1000;
  int64_t ke = ks + (end_m - start_m + 500) / 1000;
  if (ke > KM_MAX) return KMR_BAD;
  *km_start = ks;
  *km_end = ke;
  return KMR_OK;
}

bool km_live(const odometer_reading_t *r, size_t n, int64_t car_id, int64_t a0, int64_t a1,
             int64_t *km, int64_t *as_of) {
  const odometer_reading_t *lo = NULL, *hi = NULL;
  for (size_t i = 0; i < n; i++) {
    const odometer_reading_t *x = &r[i];
    if (x->car_id != car_id || x->ts < a0 || x->ts > a1) continue;
    if (!lo || x->ts < lo->ts) lo = x;
    if (!hi || x->ts > hi->ts) hi = x;
  }
  if (!lo || lo->odometer_m < 0 || hi->odometer_m > M_MAX || hi->odometer_m < lo->odometer_m) return false;
  int64_t k = (hi->odometer_m - lo->odometer_m + 500) / 1000;
  if (k > KM_MAX) return false;
  *km = k;
  *as_of = hi->ts;
  return true;
}
