#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "core/igloo.h"

#define T 1774742400
#define H 3600
#define W 1795179600

void setUp(void) {}
void tearDown(void) {}

static void test_iso_hour(void) {
    char b[32];
    TEST_ASSERT_EQUAL_INT(0, igloo_iso_hour(T, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("2026-03-29T01:00:00+01:00", b);
    TEST_ASSERT_EQUAL_INT(0, igloo_iso_hour(T + H, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("2026-03-29T03:00:00+02:00", b);
    TEST_ASSERT_EQUAL_INT(0, igloo_iso_hour(W, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("2026-11-20T14:00:00+01:00", b);
    TEST_ASSERT_EQUAL_INT(-1, igloo_iso_hour(W + 60, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, igloo_iso_hour(W, b, 25)); /* needs 26 */
    TEST_ASSERT_EQUAL_INT(0, igloo_iso_hour(W, b, 26));
}

static void test_hourly_body(void) {
    char *s = igloo_hourly_body(W, W + 4 * H, 2, "Buchung 12");
    TEST_ASSERT_NOT_NULL(s);
    cJSON *j = cJSON_Parse(s);
    free(s);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItemCaseSensitive(j, "variance")->valueint);
    TEST_ASSERT_EQUAL_STRING("2026-11-20T14:00:00+01:00", cJSON_GetObjectItemCaseSensitive(j, "startDate")->valuestring);
    TEST_ASSERT_EQUAL_STRING("2026-11-20T18:00:00+01:00", cJSON_GetObjectItemCaseSensitive(j, "endDate")->valuestring);
    TEST_ASSERT_EQUAL_STRING("Buchung 12", cJSON_GetObjectItemCaseSensitive(j, "accessName")->valuestring);
    cJSON_Delete(j);
    TEST_ASSERT_NULL(igloo_hourly_body(W, W + 4 * H, 0, "x"));
    TEST_ASSERT_NULL(igloo_hourly_body(W, W + 4 * H, 4, "x"));
    TEST_ASSERT_NULL(igloo_hourly_body(W + 60, W + 4 * H, 1, "x"));
}

static void test_parse_pin(void) {
    pin_result_t r;
    TEST_ASSERT_EQUAL_INT(0, igloo_parse_pin("{\"pin\":\"5729314\",\"pinId\":\"AA9876B22D5E405C\"}", &r));
    TEST_ASSERT_EQUAL_STRING("5729314", r.pin);
    TEST_ASSERT_EQUAL_STRING("AA9876B22D5E405C", r.pin_id);
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("{\"pin\":\"57a9314\",\"pinId\":\"X\"}", &r));
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("{\"pin\":\"123\",\"pinId\":\"X\"}", &r));
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("{\"pin\":\"1234567890123\",\"pinId\":\"X\"}", &r));
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("{\"pin\":\"1234567\"}", &r));
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("not json", &r));
}

static void test_parse_token(void) {
    char tok[16];
    int64_t exp = 0;
    const char *ok = "{\"access_token\":\"abc\",\"expires_in\":86400,\"token_type\":\"Bearer\"}";
    TEST_ASSERT_EQUAL_INT(0, igloo_parse_token(ok, tok, sizeof tok, &exp));
    TEST_ASSERT_EQUAL_STRING("abc", tok);
    TEST_ASSERT_EQUAL_INT64(86400, exp);
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_token(ok, tok, 3, &exp)); /* cap too small */
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_token("{\"expires_in\":5}", tok, sizeof tok, &exp));
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_token("junk", tok, sizeof tok, &exp));
}

static void test_parse_devices(void) {
    lock_info_t d[4];
    size_t n = 99;
    TEST_ASSERT_EQUAL_INT(0, igloo_parse_devices("[{\"deviceId\":\"D1\",\"deviceName\":\"D\xc3\xb6rpsmobil 1\"}]", d, 4, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_STRING("D1", d[0].device_id);
    TEST_ASSERT_EQUAL_STRING("D\xc3\xb6rpsmobil 1", d[0].name);
    TEST_ASSERT_EQUAL_INT(0, igloo_parse_devices("{\"payload\":[{\"id\":\"D2\",\"name\":\"B\"},{\"deviceId\":\"bad/id\"}]}", d, 4, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_STRING("D2", d[0].device_id);
    TEST_ASSERT_EQUAL_STRING("B", d[0].name);
    TEST_ASSERT_EQUAL_INT(0, igloo_parse_devices("{\"devices\":[{\"deviceId\":\"D3\"}]}", d, 4, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_STRING("", d[0].name);
    TEST_ASSERT_EQUAL_INT(0, igloo_parse_devices("{\"data\":[{\"deviceId\":\"D4\"}]}", d, 4, &n));
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_devices("{\"x\":1}", d, 4, &n));
    TEST_ASSERT_EQUAL_INT(-1, igloo_parse_devices("junk", d, 4, &n));
    TEST_ASSERT_EQUAL_INT(0, igloo_parse_devices("[{\"deviceId\":\"A\"},{\"deviceId\":\"B\"},{\"deviceId\":\"C\"}]", d, 2, &n));
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_STRING("B", d[1].device_id);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_iso_hour);
    RUN_TEST(test_hourly_body);
    RUN_TEST(test_parse_pin);
    RUN_TEST(test_parse_token);
    RUN_TEST(test_parse_devices);
    return UNITY_END();
}
