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
  char tracker_imei[16]; /* "" = no tracker, else 15 ASCII digits */
  char lock_device_id[64]; /* "" = no key box, else igloohome device ID (lock_device_id_valid) */ } car_t;
typedef struct {
  int64_t id, car_id, user_id, booked_start, booked_end; booking_state_t state;
  int64_t cancelled_at, cancel_fee_cents, created_at, modified_at, modified_by;
  bool club_trip;
  int64_t actual_start, actual_end, km_start, km_end; /* actual_*: 0 = unset */
  km_source_t km_source; bool km_pending;
  int64_t invoice_id;  /* 0 = not invoiced */
  bool start_reminded, late_reminded, end_reminded;  /* reminder sent (push or mail); default false */
} booking_t;
typedef struct {
  int64_t invoice_number, booking_id;
  char car_name[64];
  int64_t start, end;                 /* actual if started, else booked; cancelled: booked */
  bool never_started, cancelled, club_trip;
  int64_t day_min, night_min, unused_min, km;
  int64_t time_cents, km_cents, cap_cut_cents;   /* cap_cut = time_cap_cut + combined_cap_cut */
  int64_t late_fee_cents, cancel_fee_cents, total_cents;
} invoice_line_t;
typedef struct {
  int64_t id, booking_id, user_id, created_at; damage_phase_t phase;
  char text[1024]; int n_photos;
} damage_report_t;
typedef struct { int64_t car_id, ts, odometer_m, received_at; } odometer_reading_t;
typedef struct { char token_hash[65]; int64_t user_id, expires_at; } session_t;
typedef struct { char token_hash[65]; int64_t user_id, expires_at; token_kind_t kind; bool used; } token_t;
/* Named tariff; its prices live in tariff_version_t. name: 1..63 bytes. */
typedef struct { int64_t id; char name[64]; int64_t created_at; } tariff_t;
/* Versioned price list of one tariff; the version in force is the largest valid_from <= ts.
 * Money in cents; night_*: minutes after local midnight; caps 0 = no cap. */
typedef struct {
  int64_t id, tariff_id, valid_from;
  int64_t rate_day_cents_per_h, rate_night_cents_per_h, rate_km_cents;
  int night_start, night_end;          /* 0..1439 */
  int unused_factor_permille;          /* 0..1000 */
  int64_t time_cap_cents_per_day, combined_cap_cents_per_day, late_fee_cents_per_started_h;
  int64_t created_at;
} tariff_version_t;
/* Assignment of a tariff to a user; the one in force is the largest valid_from <= ts.
 * created_by: id of the admin user who made it (0 = system). */
typedef struct { int64_t id, user_id, tariff_id, valid_from, created_at, created_by; } user_tariff_t;
typedef struct {
  int start_tolerance_min, cancel_deadline_min;
  int64_t cancel_fee_cents;
  char assoc_name[96], assoc_street[96], assoc_zip[16], assoc_city[64];
  char creditor_id[36], creditor_iban[35], creditor_bic[12];
  int sepa_lead_days;                  /* default 5 */
  int reminder_before_min;             /* 0..1440, default 60, 0 = off */
  int end_reminder_before_min;         /* 0..1440, default 30, 0 = off */
  char invoice_footer[512];
} settings_t;
typedef enum { MANDATE_ACTIVE = 0, MANDATE_REPLACED, MANDATE_REVOKED } mandate_status_t;
typedef struct {
  int64_t id, user_id, created_at;      /* created_at = mandate_date */
  int64_t ended_at, ended_by;           /* 0 while active; ended_by = acting user id */
  mandate_status_t status;
  char holder[256], iban[35], bic[12];  /* bic "" = none */
  char client_ip[64]; int text_version;
  char creditor_name[96], creditor_id[36];  /* snapshot at creation */
} mandate_t;

typedef enum { RUN_DRAFT = 0, RUN_FINAL } run_state_t;
typedef struct {
  int64_t id, created_at, finalized_at, sent_at;   /* 0 = not yet */
  char period[8]; run_state_t state;
  char collection_date[11];                        /* "" until first send */
  char sepa_file[64];                              /* relative to data dir; "" none */
} billing_run_t;
typedef struct {
  int64_t number, user_id, run_id, total_cents, n_lines;
  int64_t created_at, issued_at, mailed_at, mandate_id;  /* issued/mailed/mandate 0 until send */
  char period[8];
  char name[96], street[96], zip[16], city[64];          /* addressee at finalize */
} invoice_t;

typedef struct { int64_t id, booking_id, car_id; char pin_id[64]; char pin[16];
  int64_t valid_from, valid_until; int variance; int64_t created_at; } access_pin_t;
/* booking_id 0 = admin test PIN; pin "" = hidden or test */

/* Web Push subscription of one browser (spec §10). endpoint unique; p256dh/auth base64url as the
 * browser sends them (65 / 16 bytes decoded). */
#define PUSH_ENDPOINT_MAX 1024
typedef struct { int64_t id, user_id, created_at; char endpoint[PUSH_ENDPOINT_MAX];
  char p256dh[96]; char auth[32]; } push_sub_t;

#endif
