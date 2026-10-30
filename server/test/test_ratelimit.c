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

/* I2: a full table must never evict an entry that is still blocking. */
void test_rl_block_survives_flood(void) {
  char k[32];
  for (int j = 0; j < 5; j++) rl_fail(rl, "A", 0);
  for (int i = 0; i < 300; i++) {
    snprintf(k, sizeof k, "k%d", i);
    rl_fail(rl, k, 1 + i);
  }
  TEST_ASSERT_FALSE(rl_allowed(rl, "A", 301));
  TEST_ASSERT_TRUE(rl_allowed(rl, "A", 900));
}

void test_rl_full_evicts_expired_first(void) {
  char k[32];
  for (int j = 0; j < 4; j++) rl_fail(rl, "old", 0);
  rl_fail(rl, "x", 1000);
  for (int i = 0; i < 254; i++) {
    snprintf(k, sizeof k, "k%d", i);
    rl_fail(rl, k, 1000);
    rl_fail(rl, k, 1000);
  }
  for (int j = 0; j < 3; j++) rl_fail(rl, "x", 1000); /* x: 4 fails, table full */
  rl_fail(rl, "new", 1000);                         /* takes the expired "old" slot */
  rl_fail(rl, "x", 1001);
  TEST_ASSERT_FALSE(rl_allowed(rl, "x", 1001)); /* x was kept */
}

void test_rl_full_evicts_fewest_fails(void) {
  char k[32];
  rl_fail(rl, "y", 0);
  for (int j = 0; j < 4; j++) rl_fail(rl, "z", 0);
  for (int i = 0; i < 254; i++) {
    snprintf(k, sizeof k, "k%d", i);
    for (int j = 0; j < 3; j++) rl_fail(rl, k, 0);
  }
  rl_fail(rl, "new", 10); /* evicts y (1 fail), not z (4 fails, oldest) */
  rl_fail(rl, "z", 11);
  TEST_ASSERT_FALSE(rl_allowed(rl, "z", 11));
  rl_fail(rl, "k0", 12);
  rl_fail(rl, "k0", 12);
  TEST_ASSERT_FALSE(rl_allowed(rl, "k0", 12));
}

void test_rl_saturated_table_denies_new_keys(void) {
  char k[32];
  for (int i = 0; i < 256; i++) {
    snprintf(k, sizeof k, "k%d", i);
    for (int j = 0; j < 5; j++) rl_fail(rl, k, i);
  }
  rl_fail(rl, "k256", 300);
  TEST_ASSERT_FALSE(rl_allowed(rl, "k256", 300)); /* untracked, but reported not allowed */
  TEST_ASSERT_FALSE(rl_allowed(rl, "fresh", 300));
  TEST_ASSERT_FALSE(rl_allowed(rl, "k0", 300));  /* never evicted */
  TEST_ASSERT_TRUE(rl_allowed(rl, "fresh", 900)); /* k0 expired: a slot is free again */
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
  RUN_TEST(test_rl_block_survives_flood);
  RUN_TEST(test_rl_full_evicts_expired_first);
  RUN_TEST(test_rl_full_evicts_fewest_fails);
  RUN_TEST(test_rl_saturated_table_denies_new_keys);
  RUN_TEST(test_rl_window_expiry_restarts);
  return UNITY_END();
}
