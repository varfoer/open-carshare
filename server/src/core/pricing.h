#ifndef OCS_CORE_PRICING_H
#define OCS_CORE_PRICING_H

#include <stdint.h>
#include "core/model.h"

typedef enum { PRICE_OK = 0, PRICE_KM_PENDING, PRICE_NO_TARIFF } price_status_t;
typedef struct {
  price_status_t status;
  int64_t day_min, night_min;               /* active minutes by rate */
  int64_t unused_day_min, unused_night_min;
  int days;                                 /* local calendar days touched by span */
  int64_t time_cents;                       /* after per-day time cap */
  int64_t time_cap_cut_cents;               /* >= 0: sum(day cost - capped day cost) */
  int64_t km, km_cents;
  int64_t combined_cap_cut_cents;           /* >= 0 */
  int64_t late_min, late_fee_cents, cancel_fee_cents;
  int64_t total_cents;  /* OK: full total. KM_PENDING: time_cents + late_fee (no km, no combined cap). NO_TARIFF: 0 */
} price_t;

/* Spec section 6 formula, pure integer, O(local days). Times are truncated to whole
 * minutes. a0 == 0: never started. km < 0: km unknown (KM_PENDING). */
void price_compute(const tariff_version_t *t, int tolerance_min, int64_t b0, int64_t b1,
                   int64_t a0, int64_t a1, int64_t km, price_t *out);
/* Booking wrapper: cancelled -> cancel_fee only (status OK, no tariff needed); club_trip -> all 0;
 * t == NULL otherwise -> NO_TARIFF; reserved/active -> booked time fully used, km unknown;
 * done with km_pending -> km unknown; else km = b->km. */
void price_booking(const tariff_version_t *t, const settings_t *s, const booking_t *b, price_t *out);

#endif
