#ifndef OCS_CORE_MODEL_H
#define OCS_CORE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { ROLE_USER = 0, ROLE_ADMIN = 1 } role_t;
typedef enum { BK_RESERVED = 0, BK_ACTIVE, BK_DONE, BK_CANCELLED } booking_state_t;
typedef enum { TOKEN_RESET = 0, TOKEN_INVITE = 1 } token_kind_t;
typedef enum { KM_NONE = 0, KM_TELEMATICS, KM_MANUAL } km_source_t;
typedef enum { DMG_START = 0, DMG_END = 1 } damage_phase_t;

#define DMG_MAX_PHOTOS 10

typedef struct {
  int64_t id; char email[128]; char name[96]; char street[96]; char zip[16];
  char city[64]; char pw_hash[160]; role_t role; bool active;
  int64_t license_checked_at; int64_t created_at;
  bool may_club_trip;
} user_t;
typedef struct { int64_t id; char name[64]; char plate[16]; char location[128]; bool active;
  char tracker_imei[16]; /* "" = no tracker, else 15 ASCII digits */ } car_t;
typedef struct {
  int64_t id, car_id, user_id, booked_start, booked_end; booking_state_t state;
  int64_t cancelled_at, cancel_fee_cents, created_at, modified_at, modified_by;
  bool club_trip;
  int64_t actual_start, actual_end, km_start, km_end; /* actual_*: 0 = unset */
  km_source_t km_source; bool km_pending;
} booking_t;
typedef struct {
  int64_t id, booking_id, user_id, created_at; damage_phase_t phase;
  char text[1024]; int n_photos;
} damage_report_t;
typedef struct { int64_t car_id, ts, odometer_m, received_at; } odometer_reading_t;
typedef struct { char token_hash[65]; int64_t user_id, expires_at; } session_t;
typedef struct { char token_hash[65]; int64_t user_id, expires_at; token_kind_t kind; bool used; } token_t;
/* Versioned price list of one car; the version in force is the largest valid_from <= ts.
 * Money in cents; night_*: minutes after local midnight; caps 0 = no cap. */
typedef struct {
  int64_t id, car_id, valid_from;
  int64_t rate_day_cents_per_h, rate_night_cents_per_h, rate_km_cents;
  int night_start, night_end;          /* 0..1439 */
  int unused_factor_permille;          /* 0..1000 */
  int64_t time_cap_cents_per_day, combined_cap_cents_per_day, late_fee_cents_per_started_h;
  int64_t created_at;
} tariff_t;
typedef struct { int start_tolerance_min, cancel_deadline_min; int64_t cancel_fee_cents; } settings_t;

#endif
