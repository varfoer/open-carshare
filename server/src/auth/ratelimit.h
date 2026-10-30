#ifndef OCS_AUTH_RATELIMIT_H
#define OCS_AUTH_RATELIMIT_H

#include <stdbool.h>
#include <stdint.h>

#define RL_MAX_FAILS 5
#define RL_WINDOW_SECS 900

/* Empty key = free slot. Zero-initialise the table. */
typedef struct { char key[160]; int64_t window_start; int fails; } rl_entry_t;
typedef struct { rl_entry_t e[256]; } ratelimit_t;

bool rl_allowed(const ratelimit_t *rl, const char *key, int64_t now);  /* fails < 5 within 900 s */
void rl_fail(ratelimit_t *rl, const char *key, int64_t now);           /* full table: evict oldest window_start */
void rl_reset(ratelimit_t *rl, const char *key);

#endif
