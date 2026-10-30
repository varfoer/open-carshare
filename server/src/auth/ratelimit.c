#include "auth/ratelimit.h"

#include <stddef.h>
#include <string.h>

#define N (sizeof ((ratelimit_t *)0)->e / sizeof ((ratelimit_t *)0)->e[0])

static rl_entry_t *find(const ratelimit_t *rl, const char *key) {
  if (!key || !*key) return NULL;
  for (size_t i = 0; i < N; i++)
    if (rl->e[i].key[0] && strncmp(rl->e[i].key, key, sizeof rl->e[i].key - 1) == 0) return (rl_entry_t *)&rl->e[i];
  return NULL;
}

static bool expired(const rl_entry_t *e, int64_t now) {
  return now - e->window_start >= RL_WINDOW_SECS;
}

/* Still blocking: limit reached within the current window. */
static bool blocking(const rl_entry_t *e, int64_t now) {
  return e->key[0] && !expired(e, now) && e->fails >= e->max;
}

/*
 * Slot for a new key, or NULL if every slot is blocking. Order: a free slot;
 * else the expired entry with the oldest window; else the non-blocking entry
 * with the fewest fails (oldest window on ties). A blocking entry is never
 * evicted, so failures on other keys cannot lift a block.
 */
static rl_entry_t *free_slot(ratelimit_t *rl, int64_t now) {
  rl_entry_t *exp = NULL, *few = NULL;
  for (size_t i = 0; i < N; i++) {
    rl_entry_t *e = &rl->e[i];
    if (!e->key[0]) return e;
    if (expired(e, now)) {
      if (!exp || e->window_start < exp->window_start) exp = e;
    } else if (!blocking(e, now)) {
      if (!few || e->fails < few->fails ||
          (e->fails == few->fails && e->window_start < few->window_start))
        few = e;
    }
  }
  return exp ? exp : few;
}

bool rl_allowed_n(const ratelimit_t *rl, const char *key, int64_t now, int max) {
  const rl_entry_t *e = find(rl, key);
  if (!e) return free_slot((ratelimit_t *)rl, now) != NULL; /* saturated: deny */
  if (expired(e, now)) return true;
  return e->fails < max;
}

void rl_fail_n(ratelimit_t *rl, const char *key, int64_t now, int max) {
  if (!key || !*key) return;
  rl_entry_t *e = find(rl, key);
  if (!e) {
    e = free_slot(rl, now);
    if (!e) return; /* saturated: not tracked; rl_allowed_n denies it anyway */
    memset(e, 0, sizeof *e);
    strncpy(e->key, key, sizeof e->key - 1);
    e->window_start = now;
  } else if (expired(e, now)) {
    e->window_start = now;
    e->fails = 0;
  }
  e->max = max;
  e->fails++;
}

bool rl_allowed(const ratelimit_t *rl, const char *key, int64_t now) {
  return rl_allowed_n(rl, key, now, RL_MAX_FAILS);
}

void rl_fail(ratelimit_t *rl, const char *key, int64_t now) {
  rl_fail_n(rl, key, now, RL_MAX_FAILS);
}

void rl_reset(ratelimit_t *rl, const char *key) {
  rl_entry_t *e = find(rl, key);
  if (e) memset(e, 0, sizeof *e);
}
