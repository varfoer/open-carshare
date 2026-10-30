#include <stdio.h>
#include <stdlib.h>

#include "auth/ratelimit.h"
#include "unity.h"

static ratelimit_t *rl;

void setUp(void) { rl = calloc(1, sizeof *rl); }
void tearDown(void) { free(rl); }

void test_rl_blocks_after_5_fails(void) {
  TEST_ASSERT_TRUE(rl_allowed(rl, "a", 0));
  for (int i = 0; i < 4; i++) rl_fail(rl, "a", 0);
  TEST_ASSERT_TRUE(rl_allowed(rl, "a", 0));
  rl_fail(rl, "a", 0);
  TEST_ASSERT_FALSE(rl_allowed(rl, "a", 899));
  TEST_ASSERT_TRUE(rl_allowed(rl, "a", 900));
  TEST_ASSERT_TRUE(rl_allowed(rl, "b", 0));
}

void test_rl_reset_clears(void) {
  for (int i = 0; i < 5; i++) rl_fail(rl, "a", 0);
  TEST_ASSERT_FALSE(rl_allowed(rl, "a", 1));
  rl_reset(rl, "a");
  TEST_ASSERT_TRUE(rl_allowed(rl, "a", 1));
}

void test_rl_eviction_when_full(void) {
  char k[32];
  for (int i = 0; i < 257; i++) {
    snprintf(k, sizeof k, "k%d", i);
    for (int j = 0; j < 5; j++) rl_fail(rl, k, i);
  }
  TEST_ASSERT_FALSE(rl_allowed(rl, "k256", 257));
  TEST_ASSERT_TRUE(rl_allowed(rl, "k0", 257)); /* evicted */
}

void test_rl_window_expiry_restarts(void) {
  for (int i = 0; i < 5; i++) rl_fail(rl, "a", 0);
  rl_fail(rl, "a", 1000); /* new window, 1 fail */
  TEST_ASSERT_TRUE(rl_allowed(rl, "a", 1000));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_rl_blocks_after_5_fails);
  RUN_TEST(test_rl_reset_clears);
  RUN_TEST(test_rl_eviction_when_full);
  RUN_TEST(test_rl_window_expiry_restarts);
  return UNITY_END();
}
