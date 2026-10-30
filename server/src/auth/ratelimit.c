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

bool rl_allowed(const ratelimit_t *rl, const char *key, int64_t now) {
  const rl_entry_t *e = find(rl, key);
  if (!e) return true;
  if (now - e->window_start >= RL_WINDOW_SECS) return true;
  return e->fails < RL_MAX_FAILS;
}

void rl_fail(ratelimit_t *rl, const char *key, int64_t now) {
  if (!key || !*key) return;
  rl_entry_t *e = find(rl, key);
  if (!e) {
    size_t slot = 0;
    for (size_t i = 0; i < N; i++) {
      if (!rl->e[i].key[0]) { slot = i; goto found; }
      if (rl->e[i].window_start < rl->e[slot].window_start) slot = i;
    }
  found:
    e = &rl->e[slot];
    memset(e, 0, sizeof *e);
    strncpy(e->key, key, sizeof e->key - 1);
    e->window_start = now;
  } else if (now - e->window_start >= RL_WINDOW_SECS) {
    e->window_start = now;
    e->fails = 0;
  }
  e->fails++;
}

void rl_reset(ratelimit_t *rl, const char *key) {
  rl_entry_t *e = find(rl, key);
  if (e) memset(e, 0, sizeof *e);
}
