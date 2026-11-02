#include <stdio.h>
#include <stdlib.h>
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

static void test_club_flags_roundtrip_and_default(void) {
    booking_t b = {.id = 7, .state = BK_RESERVED, .club_trip = true}, r;
    cJSON *j = booking_to_json(&b);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(j, "club_trip")));
    TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
    TEST_ASSERT_TRUE(r.club_trip);
    cJSON_Delete(j);
    b.club_trip = false;
    j = booking_to_json(&b);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItem(j, "club_trip")));
    /* old data files: key missing -> false */
    cJSON_DeleteItemFromObject(j, "club_trip");
    r.club_trip = true;
    TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
    TEST_ASSERT_FALSE(r.club_trip);
    /* wrong type is still rejected */
    cJSON_AddStringToObject(j, "club_trip", "yes");
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(j, &r));
    cJSON_Delete(j);

    user_t u = {.id = 1, .may_club_trip = true}, ur;
    j = user_to_json(&u);
    TEST_ASSERT_EQUAL_INT(0, user_from_json(j, &ur));
    TEST_ASSERT_TRUE(ur.may_club_trip);
    cJSON_Delete(j);
    j = user_to_public_json(&u);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(j, "may_club_trip")));
    cJSON_Delete(j);
    u.may_club_trip = false;
    j = user_to_json(&u);
    cJSON_DeleteItemFromObject(j, "may_club_trip");
    ur.may_club_trip = true;
    TEST_ASSERT_EQUAL_INT(0, user_from_json(j, &ur));
    TEST_ASSERT_FALSE(ur.may_club_trip);
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

static void test_car_tracker_imei_roundtrip(void) {
    car_t c = {.id = 2, .active = true};
    strcpy(c.name, "Zoe");
    strcpy(c.plate, "MS-AB 123");
    strcpy(c.location, "Hof 1");
    strcpy(c.tracker_imei, "356307042441013");
    cJSON *j = car_to_json(&c);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(j, "tracker_imei"));
    car_t r;
    TEST_ASSERT_EQUAL_INT(0, car_from_json(j, &r));
    TEST_ASSERT_EQUAL_MEMORY(&c, &r, sizeof c);
    cJSON_Delete(j);
}

static void test_car_loads_without_tracker_imei(void) {
    cJSON *j = cJSON_Parse("{\"id\":1,\"name\":\"a\",\"plate\":\"b\",\"location\":\"c\",\"active\":true}");
    car_t r;
    TEST_ASSERT_EQUAL_INT(0, car_from_json(j, &r));
    TEST_ASSERT_EQUAL_STRING("", r.tracker_imei);
    cJSON_Delete(j);
}

static void test_car_rejects_bad_imei(void) {
    const char *bad[] = {"12345", "35630704244101x", "3563070424410133"};
    for (int i = 0; i < 3; i++) {
        car_t c = {.id = 1, .active = true};
        cJSON *j = car_to_json(&c);
        cJSON_ReplaceItemInObject(j, "tracker_imei", cJSON_CreateString(bad[i]));
        car_t r;
        TEST_ASSERT_EQUAL_INT(-1, car_from_json(j, &r));
        cJSON_Delete(j);
    }
}

static void test_km_source_telematics_roundtrip(void) {
    booking_t b = {.id = 1, .booked_start = 600, .booked_end = 1200, .km_source = KM_TELEMATICS};
    cJSON *j = booking_to_json(&b);
    TEST_ASSERT_EQUAL_STRING("telematics", cJSON_GetObjectItem(j, "km_source")->valuestring);
    booking_t r;
    TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
    TEST_ASSERT_EQUAL_INT(KM_TELEMATICS, r.km_source);
    cJSON_Delete(j);
}

static void test_km_source_geotab_loads_as_telematics(void) {
    booking_t b = {.id = 1, .booked_start = 600, .booked_end = 1200};
    cJSON *j = booking_to_json(&b);
    cJSON_ReplaceItemInObject(j, "km_source", cJSON_CreateString("geotab"));
    booking_t r;
    TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
    TEST_ASSERT_EQUAL_INT(KM_TELEMATICS, r.km_source);
    cJSON_Delete(j);
}

static void test_odometer_reading_roundtrip(void) {
    odometer_reading_t v = {.car_id = 2, .ts = 1774742400, .odometer_m = 22949000,
                            .received_at = 1774742410}, r;
    cJSON *j = odometer_reading_to_json(&v);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(j));
    TEST_ASSERT_EQUAL_INT(0, odometer_reading_from_json(j, &r));
    TEST_ASSERT_EQUAL_MEMORY(&v, &r, sizeof v);
    cJSON_Delete(j);
}

static void test_odometer_reading_requires_all_keys(void) {
    const char *keys[] = {"car_id", "ts", "odometer_m", "received_at"};
    for (int i = 0; i < 4; i++) {
        odometer_reading_t v = {.car_id = 2, .ts = 5, .odometer_m = 6, .received_at = 7}, r;
        cJSON *j = odometer_reading_to_json(&v);
        cJSON_DeleteItemFromObject(j, keys[i]);
        TEST_ASSERT_EQUAL_INT(-1, odometer_reading_from_json(j, &r));
        cJSON_Delete(j);
    }
}

static void test_imei_valid(void) {
    TEST_ASSERT_TRUE(imei_valid("356307042441013"));
    TEST_ASSERT_FALSE(imei_valid(""));
    TEST_ASSERT_FALSE(imei_valid("35630704244101"));
    TEST_ASSERT_FALSE(imei_valid("3563070424410131"));
    TEST_ASSERT_FALSE(imei_valid("35630704244101x"));
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

static void test_booking_trip_roundtrip(void) {
    booking_t b = {.id = 1, .car_id = 2, .user_id = 3, .booked_start = 600, .booked_end = 1200,
                   .state = BK_DONE, .actual_start = 590, .actual_end = 1300, .km_start = 1000,
                   .km_end = 1042, .km_source = KM_MANUAL, .km_pending = false};
    cJSON *j = booking_to_json(&b);
    TEST_ASSERT_EQUAL_STRING("manual", cJSON_GetObjectItem(j, "km_source")->valuestring);
    booking_t r;
    TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
    TEST_ASSERT_EQUAL_INT64(590, r.actual_start);
    TEST_ASSERT_EQUAL_INT64(1300, r.actual_end);
    TEST_ASSERT_EQUAL_INT64(1000, r.km_start);
    TEST_ASSERT_EQUAL_INT64(1042, r.km_end);
    TEST_ASSERT_EQUAL_INT(KM_MANUAL, r.km_source);
    cJSON_Delete(j);
}

static void test_booking_loads_phase1_line(void) {
    cJSON *j = cJSON_Parse("{\"id\":1,\"car_id\":1,\"user_id\":1,\"booked_start\":600,\"booked_end\":1200,"
                           "\"state\":\"active\",\"cancelled_at\":0,\"cancel_fee_cents\":0,\"created_at\":1,"
                           "\"modified_at\":1,\"modified_by\":1}");
    booking_t r;
    TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
    TEST_ASSERT_EQUAL_INT(BK_ACTIVE, r.state);
    TEST_ASSERT_EQUAL_INT64(0, r.actual_start);
    TEST_ASSERT_EQUAL_INT(KM_NONE, r.km_source);
    TEST_ASSERT_FALSE(r.km_pending);
    cJSON_Delete(j);
}

static void test_booking_rejects_bad_km_source(void) {
    booking_t b = {.id = 1, .booked_start = 600, .booked_end = 1200};
    cJSON *j = booking_to_json(&b);
    cJSON_ReplaceItemInObject(j, "km_source", cJSON_CreateString("gps"));
    booking_t r;
    TEST_ASSERT_EQUAL_INT(-1, booking_from_json(j, &r));
    cJSON_Delete(j);
}

static void test_damage_roundtrip(void) {
    damage_report_t d = {.id = 4, .booking_id = 9, .user_id = 3, .created_at = 777,
                         .phase = DMG_END, .n_photos = 2};
    snprintf(d.text, sizeof d.text, "Kratzer an der Stoßstange, Tür links: \xC3\xBC" "berall");
    cJSON *j = damage_report_to_json(&d);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_STRING("end", cJSON_GetObjectItem(j, "phase")->valuestring);
    damage_report_t r;
    TEST_ASSERT_EQUAL_INT(0, damage_report_from_json(j, &r));
    TEST_ASSERT_EQUAL_MEMORY(&d, &r, sizeof d);
    cJSON_Delete(j);
}

static void test_damage_rejects_long_text(void) {
    damage_report_t d = {.id = 1, .booking_id = 1, .user_id = 1};
    memset(d.text, 'a', 1023);
    cJSON *j = damage_report_to_json(&d);
    TEST_ASSERT_NOT_NULL(j); /* 1023 chars is the maximum and survives untruncated */
    damage_report_t r;
    TEST_ASSERT_EQUAL_INT(0, damage_report_from_json(j, &r));
    TEST_ASSERT_EQUAL_size_t(1023, strlen(r.text));
    char *big = malloc(1025);
    memset(big, 'b', 1024);
    big[1024] = '\0';
    cJSON_ReplaceItemInObject(j, "text", cJSON_CreateString(big));
    free(big);
    TEST_ASSERT_EQUAL_INT(-1, damage_report_from_json(j, &r));
    cJSON_Delete(j);
}

static void test_damage_rejects_bad_n_photos(void) {
    damage_report_t d = {.id = 1, .booking_id = 1, .user_id = 1, .n_photos = DMG_MAX_PHOTOS};
    snprintf(d.text, sizeof d.text, "Delle");
    cJSON *j = damage_report_to_json(&d);
    TEST_ASSERT_NOT_NULL(j);
    damage_report_t r;
    TEST_ASSERT_EQUAL_INT(0, damage_report_from_json(j, &r));
    TEST_ASSERT_EQUAL_INT(DMG_MAX_PHOTOS, r.n_photos);
    cJSON_ReplaceItemInObject(j, "n_photos", cJSON_CreateNumber(DMG_MAX_PHOTOS + 1));
    TEST_ASSERT_EQUAL_INT(-1, damage_report_from_json(j, &r));
    cJSON_ReplaceItemInObject(j, "n_photos", cJSON_CreateNumber(-1));
    TEST_ASSERT_EQUAL_INT(-1, damage_report_from_json(j, &r));
    cJSON_Delete(j);
}

static tariff_t mktariff(void) {
    tariff_t t;
    memset(&t, 0, sizeof t);
    t.id = 1; t.car_id = 1; t.valid_from = 1793919600;
    t.rate_day_cents_per_h = 300; t.rate_night_cents_per_h = 150; t.rate_km_cents = 30;
    t.night_start = 1200; t.night_end = 360; t.unused_factor_permille = 500;
    t.time_cap_cents_per_day = 3000; t.combined_cap_cents_per_day = 5000;
    t.late_fee_cents_per_started_h = 1500; t.created_at = 1793900000;
    return t;
}

static void test_tariff_roundtrip(void) {
    tariff_t v = mktariff(), r;
    cJSON *j = tariff_to_json(&v);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(13, cJSON_GetArraySize(j));
    TEST_ASSERT_EQUAL_INT(0, tariff_from_json(j, &r));
    TEST_ASSERT_EQUAL_INT64(1, r.id);
    TEST_ASSERT_EQUAL_INT64(1, r.car_id);
    TEST_ASSERT_EQUAL_INT64(1793919600, r.valid_from);
    TEST_ASSERT_EQUAL_INT64(300, r.rate_day_cents_per_h);
    TEST_ASSERT_EQUAL_INT64(150, r.rate_night_cents_per_h);
    TEST_ASSERT_EQUAL_INT64(30, r.rate_km_cents);
    TEST_ASSERT_EQUAL_INT(1200, r.night_start);
    TEST_ASSERT_EQUAL_INT(360, r.night_end);
    TEST_ASSERT_EQUAL_INT(500, r.unused_factor_permille);
    TEST_ASSERT_EQUAL_INT64(3000, r.time_cap_cents_per_day);
    TEST_ASSERT_EQUAL_INT64(5000, r.combined_cap_cents_per_day);
    TEST_ASSERT_EQUAL_INT64(1500, r.late_fee_cents_per_started_h);
    TEST_ASSERT_EQUAL_INT64(1793900000, r.created_at);
    cJSON_Delete(j);
}

static void test_tariff_rejects_out_of_range(void) {
    tariff_t v = mktariff(), r;
    v.night_start = 1440;
    TEST_ASSERT_FALSE(tariff_valid(&v));
    cJSON *j = tariff_to_json(&v);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(-1, tariff_from_json(j, &r));
    cJSON_Delete(j);
    v = mktariff();
    v.unused_factor_permille = 1001;
    j = tariff_to_json(&v);
    TEST_ASSERT_EQUAL_INT(-1, tariff_from_json(j, &r));
    cJSON_Delete(j);
    v = mktariff();
    v.rate_km_cents = -1;
    j = tariff_to_json(&v);
    TEST_ASSERT_EQUAL_INT(-1, tariff_from_json(j, &r));
    cJSON_Delete(j);
    v = mktariff();
    v.late_fee_cents_per_started_h = 10000001;
    TEST_ASSERT_FALSE(tariff_valid(&v));
}

static void test_tariff_requires_all_keys(void) {
    const char *keys[] = {"id", "car_id", "valid_from", "rate_day_cents_per_h",
                          "rate_night_cents_per_h", "rate_km_cents", "night_start", "night_end",
                          "unused_factor_permille", "time_cap_cents_per_day",
                          "combined_cap_cents_per_day", "late_fee_cents_per_started_h",
                          "created_at"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        tariff_t v = mktariff(), r;
        cJSON *j = tariff_to_json(&v);
        cJSON_DeleteItemFromObject(j, keys[i]);
        TEST_ASSERT_EQUAL_INT(-1, tariff_from_json(j, &r));
        cJSON_Delete(j);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_booking_roundtrip);
    RUN_TEST(test_club_flags_roundtrip_and_default);
    RUN_TEST(test_booking_trip_roundtrip);
    RUN_TEST(test_booking_loads_phase1_line);
    RUN_TEST(test_booking_rejects_bad_km_source);
    RUN_TEST(test_damage_roundtrip);
    RUN_TEST(test_damage_rejects_long_text);
    RUN_TEST(test_damage_rejects_bad_n_photos);
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
    RUN_TEST(test_car_tracker_imei_roundtrip);
    RUN_TEST(test_car_loads_without_tracker_imei);
    RUN_TEST(test_car_rejects_bad_imei);
    RUN_TEST(test_km_source_telematics_roundtrip);
    RUN_TEST(test_km_source_geotab_loads_as_telematics);
    RUN_TEST(test_odometer_reading_roundtrip);
    RUN_TEST(test_odometer_reading_requires_all_keys);
    RUN_TEST(test_imei_valid);
    RUN_TEST(test_session_roundtrip);
    RUN_TEST(test_token_roundtrip);
    RUN_TEST(test_settings_defaults_when_missing);
    RUN_TEST(test_settings_roundtrip_and_wrong_type);
    RUN_TEST(test_tariff_roundtrip);
    RUN_TEST(test_tariff_rejects_out_of_range);
    RUN_TEST(test_tariff_requires_all_keys);
    return UNITY_END();
}
