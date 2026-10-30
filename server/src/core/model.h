#ifndef OCS_CORE_MODEL_H
#define OCS_CORE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { ROLE_USER = 0, ROLE_ADMIN = 1 } role_t;
typedef enum { BK_RESERVED = 0, BK_ACTIVE, BK_DONE, BK_CANCELLED } booking_state_t;
typedef enum { TOKEN_RESET = 0, TOKEN_INVITE = 1 } token_kind_t;

typedef struct {
  int64_t id; char email[128]; char name[96]; char street[96]; char zip[16];
  char city[64]; char pw_hash[160]; role_t role; bool active;
  int64_t license_checked_at; int64_t created_at;
} user_t;
typedef struct { int64_t id; char name[64]; char plate[16]; char location[128]; bool active; } car_t;
typedef struct {
  int64_t id, car_id, user_id, booked_start, booked_end; booking_state_t state;
  int64_t cancelled_at, cancel_fee_cents, created_at, modified_at, modified_by;
} booking_t;
typedef struct { char token_hash[65]; int64_t user_id, expires_at; } session_t;
typedef struct { char token_hash[65]; int64_t user_id, expires_at; token_kind_t kind; bool used; } token_t;
typedef struct { int start_tolerance_min, cancel_deadline_min; int64_t cancel_fee_cents; } settings_t;

#endif
