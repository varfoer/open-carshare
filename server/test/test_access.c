#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "core/access.h"

#define T 1774742400
#define H 3600

void setUp(void) {}
void tearDown(void) {}

static void test_window_examples(void) {
    int64_t f, u;
    access_window(T + 1200, T + 13500, &f, &u);
    TEST_ASSERT_EQUAL_INT64(T, f); TEST_ASSERT_EQUAL_INT64(T + 4 * H, u);
    access_window(T + 3540, T + H, &f, &u);
    TEST_ASSERT_EQUAL_INT64(T, f); TEST_ASSERT_EQUAL_INT64(T + 2 * H, u);
    access_window(T + 10 * H + 600, T + 5 * H, &f, &u); /* overdue */
    TEST_ASSERT_EQUAL_INT64(T + 10 * H, f); TEST_ASSERT_EQUAL_INT64(T + 12 * H, u);
    access_window(T + H, T + 5 * H, &f, &u);
    TEST_ASSERT_EQUAL_INT64(T + 5 * H, u); /* end on full hour */
    access_window(T, T + 800 * H, &f, &u);
    TEST_ASSERT_EQUAL_INT64(T + 672 * H, u); /* cap */
}

static void test_usable_margin(void) {
    access_pin_t p = {.pin = "1234567", .valid_until = T + 4 * H};
    TEST_ASSERT_TRUE(access_usable(&p, T + 4 * H - 901));
    TEST_ASSERT_FALSE(access_usable(&p, T + 4 * H - 900));
    p.pin[0] = 0;
    TEST_ASSERT_FALSE(access_usable(&p, T));
}

static void test_device_id_valid(void) {
    char s[80];
    TEST_ASSERT_TRUE(lock_device_id_valid("SP2X1234ab"));
    TEST_ASSERT_TRUE(lock_device_id_valid("a.b_c-d"));
    TEST_ASSERT_FALSE(lock_device_id_valid(""));
    TEST_ASSERT_FALSE(lock_device_id_valid("a/b"));
    TEST_ASSERT_FALSE(lock_device_id_valid("a b"));
    TEST_ASSERT_FALSE(lock_device_id_valid("a?b"));
    TEST_ASSERT_FALSE(lock_device_id_valid("a%b"));
    TEST_ASSERT_FALSE(lock_device_id_valid("a#b"));
    memset(s, 'a', 64); s[64] = 0;
    TEST_ASSERT_FALSE(lock_device_id_valid(s));
    s[63] = 0;
    TEST_ASSERT_TRUE(lock_device_id_valid(s));
}

static void test_err_texts(void) {
    TEST_ASSERT_EQUAL_STRING("In der igloohome-App der Box eine Zeitzone (Europe/Berlin) zuweisen.",
                             pin_err_admin_de(PIN_ERR_TIMEZONE));
    TEST_ASSERT_EQUAL_STRING("igloo: device property has no timezone", pin_err_log(PIN_ERR_TIMEZONE));
    TEST_ASSERT_EQUAL_STRING("Die igloohome-Zugangsdaten fehlen in ocs.conf.", pin_err_admin_de(PIN_ERR_CONFIG));
    TEST_ASSERT_EQUAL_STRING("igloo: unexpected answer", pin_err_log(PIN_ERR_OTHER));
    for (int e = PIN_ERR_CONFIG; e <= PIN_ERR_OTHER; e++) {
        TEST_ASSERT_NOT_NULL(pin_err_admin_de((pin_err_t)e));
        TEST_ASSERT_NOT_NULL(pin_err_log((pin_err_t)e));
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_window_examples);
    RUN_TEST(test_usable_margin);
    RUN_TEST(test_device_id_valid);
    RUN_TEST(test_err_texts);
    return UNITY_END();
}
