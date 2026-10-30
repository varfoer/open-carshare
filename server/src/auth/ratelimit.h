#ifndef OCS_AUTH_RATELIMIT_H
#define OCS_AUTH_RATELIMIT_H

#include <stdbool.h>
#include <stdint.h>

#define RL_MAX_FAILS 5
#define RL_WINDOW_SECS 900

/* Empty key = free slot. Zero-initialise the table. max = limit of the last
 * rl_fail_n on this key; the entry is "blocking" while fails >= max within
 * its 900 s window. */
typedef struct { char key[160]; int64_t window_start; int fails; int max; } rl_entry_t;
typedef struct { rl_entry_t e[256]; } ratelimit_t;

/* Allowed while fails < max within 900 s. A key not in the table is allowed
 * unless the table is saturated (every slot blocking): then it is denied. */
bool rl_allowed_n(const ratelimit_t *rl, const char *key, int64_t now, int max);
/* Counts one event. A new key in a full table takes an expired slot, else the
 * non-blocking slot with the fewest fails; blocking entries are never evicted.
 * If every slot is blocking, the key is not tracked (rl_allowed_n denies it). */
void rl_fail_n(ratelimit_t *rl, const char *key, int64_t now, int max);
bool rl_allowed(const ratelimit_t *rl, const char *key, int64_t now);  /* max RL_MAX_FAILS */
void rl_fail(ratelimit_t *rl, const char *key, int64_t now);           /* max RL_MAX_FAILS */
void rl_reset(ratelimit_t *rl, const char *key);

#endif
