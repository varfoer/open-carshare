#include <string.h>
#include "unity.h"
#include "helpers.h"
#include "cJSON.h"
#include "core/model.h"
#include "core/codec.h"

void setUp(void) {}
void tearDown(void) {}

static void test_booking_roundtrip(void) {
    booking_t b = {.id = 7, .car_id = 1, .user_id = 2, .booked_start = 1774742400,
                   .booked_end = 1774746000, .state = BK_RESERVED, .created_at = 1};
    cJSON *j = booking_to_json(&b);
    booking_t r;
    TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
    TEST_ASSERT_EQUAL_MEMORY(&b, &r, sizeof b);
    TEST_ASSERT_EQUAL_STRING("reserved", cJSON_GetObjectItem(j, "state")->valuestring);
    cJSON_Delete(j);
}

static void test_booking_all_states(void) {
    booking_state_t st[] = {BK_RESERVED, BK_ACTIVE, BK_DONE, BK_CANCELLED};
    const char *names[] = {"reserved", "active", "done", "cancelled"};
    for (int i = 0; i < 4; i++) {
        booking_t b = {.id = 1, .state = st[i]};
        cJSON *j = booking_to_json(&b);
        TEST_ASSERT_EQUAL_STRING(names[i], cJSON_GetObjectItem(j, "state")->valuestring);
        booking_t r;
        TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
        TEST_ASSERT_EQUAL_INT(st[i], r.state);
        cJSON_Delete(j);
    }
}

static void test_user_umlaut_roundtrip(void) {
    user_t u = {.id = 3, .role = ROLE_ADMIN, .active = true, .created_at = 5};
    strcpy(u.email, "j@example.de");
    strcpy(u.name, "J\xc3\xbcrgen M\xc3\xbc\xc3\x9fig");
    strcpy(u.pw_hash, "hash");
    cJSON *j = user_to_json(&u);
    user_t r;
    TEST_ASSERT_EQUAL_INT(0, user_from_json(j, &r));
    TEST_ASSERT_EQUAL_STRING("J\xc3\xbcrgen M\xc3\xbc\xc3\x9fig", r.name);
    TEST_ASSERT_EQUAL_MEMORY(&u, &r, sizeof u);
    TEST_ASSERT_EQUAL_STRING("admin", cJSON_GetObjectItem(j, "role")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(j, "active")));
    cJSON_Delete(j);
}

static void test_user_public_json_has_no_pw_hash(void) {
    user_t u = {.id = 3};
    strcpy(u.email, "a@b.de");
    strcpy(u.pw_hash, "secret");
    cJSON *j = user_to_public_json(&u);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(j, "pw_hash"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(j, "email"));
    cJSON_Delete(j);
}

static void test_from_json_rejects_too_long_email(void) {
    user_t u = {.id = 1};
    cJSON *j = user_to_json(&u);
    char big[129];
    memset(big, 'a', 128);
    big[128] = 0;
    cJSON_ReplaceItemInObject(j, "email", cJSON_CreateString(big));
    user_t r;
    TEST_ASSERT_EQUAL_INT(-1, user_from_json(j, &r));
    big[127] = 0; /* 127 chars fits */
    cJSON_ReplaceItemInObject(j, "email", cJSON_CreateString(big));
    TEST_ASSERT_EQUAL_INT(0, user_from_json(j, &r));
    cJSON_Delete(j);
}

static void test_from_json_rejects_wrong_type(void) {
    booking_t b = {.id = 7};
    cJSON *j = booking_to_json(&b);
    cJSON_ReplaceItemInObject(j, "id", cJSON_CreateString("7"));
    booking_t r;
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(j, &r));
    cJSON_Delete(j);
}

static void test_from_json_rejects_missing_field(void) {
    booking_t b = {.id = 7};
    cJSON *j = booking_to_json(&b);
    cJSON_DeleteItemFromObject(j, "car_id");
    booking_t r;
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(j, &r));
    cJSON_Delete(j);
}

static void test_from_json_rejects_non_integral_and_out_of_range(void) {
    booking_t b = {.id = 7};
    booking_t r;
    cJSON *j = booking_to_json(&b);
    cJSON_ReplaceItemInObject(j, "id", cJSON_CreateNumber(1.5));
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(j, &r));
    cJSON_ReplaceItemInObject(j, "id", cJSON_CreateNumber(9007199254740994.0));
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(j, &r));
    cJSON_ReplaceItemInObject(j, "id", cJSON_CreateNumber(-9007199254740994.0));
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(j, &r));
    cJSON_ReplaceItemInObject(j, "id", cJSON_CreateNumber(9007199254740992.0));
    TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
    cJSON_Delete(j);
}

static void test_from_json_rejects_bad_enum(void) {
    booking_t b = {.id = 7};
    booking_t r;
    cJSON *j = booking_to_json(&b);
    cJSON_ReplaceItemInObject(j, "state", cJSON_CreateString("bogus"));
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(j, &r));
    cJSON_Delete(j);
}

static void test_from_json_rejects_non_object(void) {
    booking_t r;
    cJSON *j = cJSON_CreateArray();
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(j, &r));
    cJSON_Delete(j);
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(NULL, &r));
}

static void test_from_json_ignores_unknown_keys(void) {
    booking_t b = {.id = 7, .state = BK_DONE};
    cJSON *j = booking_to_json(&b);
    cJSON_AddStringToObject(j, "future_field", "x");
    cJSON_AddNumberToObject(j, "other", 3);
    booking_t r;
    TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
    TEST_ASSERT_EQUAL_MEMORY(&b, &r, sizeof b);
    cJSON_Delete(j);
}

static void test_car_roundtrip(void) {
    car_t c = {.id = 2, .active = true};
    strcpy(c.name, "Zoe");
    strcpy(c.plate, "MS-AB 123");
    strcpy(c.location, "Hof 1");
    cJSON *j = car_to_json(&c);
    car_t r;
    TEST_ASSERT_EQUAL_INT(0, car_from_json(j, &r));
    TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
    cJSON_Delete(j);
}

static void test_session_roundtrip(void) {
    session_t s = {.user_id = 4, .expires_at = 99};
    memset(s.token_hash, 'a', 64);
    cJSON *j = session_to_json(&s);
    session_t r;
    TEST_ASSERT_EQUAL_INT(0, session_from_json(j, &r));
    TEST_ASSERT_EQUAL_MEMORY(&s, &r, sizeof s);
    cJSON_Delete(j);
}

static void test_token_roundtrip(void) {
    token_t t = {.user_id = 4, .expires_at = 99, .kind = TOKEN_INVITE, .used = true};
    memset(t.token_hash, 'b', 64);
    cJSON *j = token_to_json(&t);
    token_t r;
    TEST_ASSERT_EQUAL_STRING("invite", cJSON_GetObjectItem(j, "kind")->valuestring);
    TEST_ASSERT_EQUAL_INT(0, token_from_json(j, &r));
    TEST_ASSERT_EQUAL_MEMORY(&t, &r, sizeof t);
    cJSON_Delete(j);
}

static void test_settings_defaults_when_missing(void) {
    cJSON *j = cJSON_Parse("{}");
    settings_t s;
    TEST_ASSERT_EQUAL_INT(0, settings_from_json(j, &s));
    TEST_ASSERT_EQUAL_INT(15, s.start_tolerance_min);
    TEST_ASSERT_EQUAL_INT(0, s.cancel_deadline_min);
    TEST_ASSERT_EQUAL_INT64(0, s.cancel_fee_cents);
    cJSON_Delete(j);
}

static void test_settings_roundtrip_and_wrong_type(void) {
    settings_t s = {.start_tolerance_min = 5, .cancel_deadline_min = 60, .cancel_fee_cents = 250};
    cJSON *j = settings_to_json(&s);
    settings_t r;
    TEST_ASSERT_EQUAL_INT(0, settings_from_json(j, &r));
    TEST_ASSERT_EQUAL_MEMORY(&s, &r, sizeof s);
    cJSON_ReplaceItemInObject(j, "cancel_fee_cents", cJSON_CreateString("x"));
    TEST_ASSERT_EQUAL_INT(-1, settings_from_json(j, &r));
    cJSON_Delete(j);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_booking_roundtrip);
    RUN_TEST(test_booking_all_states);
    RUN_TEST(test_user_umlaut_roundtrip);
    RUN_TEST(test_user_public_json_has_no_pw_hash);
    RUN_TEST(test_from_json_rejects_too_long_email);
    RUN_TEST(test_from_json_rejects_wrong_type);
    RUN_TEST(test_from_json_rejects_missing_field);
    RUN_TEST(test_from_json_rejects_non_integral_and_out_of_range);
    RUN_TEST(test_from_json_rejects_bad_enum);
    RUN_TEST(test_from_json_rejects_non_object);
    RUN_TEST(test_from_json_ignores_unknown_keys);
    RUN_TEST(test_car_roundtrip);
    RUN_TEST(test_session_roundtrip);
    RUN_TEST(test_token_roundtrip);
    RUN_TEST(test_settings_defaults_when_missing);
    RUN_TEST(test_settings_roundtrip_and_wrong_type);
    return UNITY_END();
}
