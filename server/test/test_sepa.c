#include <string.h>
#include "unity.h"
#include "core/sepa.h"

void setUp(void) {}
void tearDown(void) {}

static void test_normalize_iban_spaces_lowercase(void) {
    char out[64], small[35];
    TEST_ASSERT_EQUAL_INT(0, sepa_normalize("de89 3704 0044 0532 0130 00", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("DE89370400440532013000", out);
    TEST_ASSERT_EQUAL_INT(-1, sepa_normalize("DE89-3704", out, sizeof out));
    TEST_ASSERT_EQUAL_INT(-1, sepa_normalize("1234567890123456789012345678901234567890", small, sizeof small));
}

static void test_iban_valid_vectors(void) {
    TEST_ASSERT_TRUE(iban_valid("DE89370400440532013000"));
    TEST_ASSERT_TRUE(iban_valid("GB82WEST12345698765432"));
    TEST_ASSERT_TRUE(iban_valid("NL91ABNA0417164300"));
    TEST_ASSERT_TRUE(iban_valid("AT611904300234573201"));
    TEST_ASSERT_FALSE(iban_valid("DE89370400440532013001"));
    TEST_ASSERT_FALSE(iban_valid("DE8937040044053201300"));
    TEST_ASSERT_FALSE(iban_valid("US64SVBKUS6S3300958879"));
    TEST_ASSERT_FALSE(iban_valid(""));
}

static void test_bic_valid(void) {
    TEST_ASSERT_TRUE(bic_valid("COBADEFFXXX"));
    TEST_ASSERT_TRUE(bic_valid("COBADEFF"));
    TEST_ASSERT_FALSE(bic_valid("COBADEF"));
    TEST_ASSERT_FALSE(bic_valid("COBADEFFXX"));
    TEST_ASSERT_FALSE(bic_valid("1OBADEFF"));
    TEST_ASSERT_FALSE(bic_valid(""));
}

static void test_creditor_id_valid(void) {
    TEST_ASSERT_TRUE(creditor_id_valid("DE98ZZZ09999999999"));
    TEST_ASSERT_FALSE(creditor_id_valid("DE98ZZZ09999999998"));
    TEST_ASSERT_FALSE(creditor_id_valid("DE98ZZZ0999999999"));
    TEST_ASSERT_FALSE(creditor_id_valid(""));
}

static void test_iban_mask(void) {
    char out[32];
    iban_mask("DE89370400440532013000", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("DE89 **** 3000", out);
    iban_mask("DE89", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_mandate_ref(void) {
    char out[32];
    mandate_ref(12, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("OCS-000012", out);
    mandate_ref(1234567, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("OCS-1234567", out);
}

static void test_mandate_text_fills_creditor(void) {
    char out[2048];
    int n = mandate_text(out, sizeof out, "Carsharing Testverein e.V.", "DE98ZZZ09999999999");
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_INT((int)strlen(out), n);
    static const char golden[] =
        "SEPA-Lastschriftmandat\n\n"
        "Zahlungsempfänger: Carsharing Testverein e.V.\n"
        "Gläubiger-Identifikationsnummer: DE98ZZZ09999999999\n"
        "Mandatsreferenz: wird nach Erteilung des Mandats mitgeteilt\n\n"
        "Ich ermächtige Carsharing Testverein e.V., Zahlungen von meinem Konto mittels Lastschrift einzuziehen. "
        "Zugleich weise ich mein Kreditinstitut an, die von Carsharing Testverein e.V. auf mein Konto gezogenen Lastschriften einzulösen.\n\n"
        "Hinweis: Ich kann innerhalb von acht Wochen, beginnend mit dem Belastungsdatum, die Erstattung des belasteten Betrages verlangen. "
        "Es gelten dabei die mit meinem Kreditinstitut vereinbarten Bedingungen.\n\n"
        "Das Mandat gilt für wiederkehrende Lastschriften.";
    TEST_ASSERT_EQUAL_STRING(golden, out);
    TEST_ASSERT_NOT_NULL(strstr(out, "Ich ermächtige Carsharing Testverein e.V., Zahlungen"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Gläubiger-Identifikationsnummer: DE98ZZZ09999999999"));
    TEST_ASSERT_EQUAL_INT(-1, mandate_text(out, 10, "Carsharing Testverein e.V.", "DE98ZZZ09999999999"));
}

static void test_mandate_mail_body(void) {
    mandate_t m = {.id = 3, .user_id = 7};
    snprintf(m.holder, sizeof m.holder, "Max Mustermann");
    snprintf(m.iban, sizeof m.iban, "DE89370400440532013000");
    snprintf(m.creditor_name, sizeof m.creditor_name, "Carsharing Testverein e.V.");
    snprintf(m.creditor_id, sizeof m.creditor_id, "DE98ZZZ09999999999");
    char out[4096];
    int n = mandate_mail_body(out, sizeof out, "Erika", &m);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_size_t((size_t)n, strlen(out));
    TEST_ASSERT_EQUAL_INT(0, strncmp(out, "Hallo Erika,\n\ndu hast online ein SEPA-Lastschriftmandat erteilt.\n\n", 63));
    TEST_ASSERT_NOT_NULL(strstr(out, "Mandatsreferenz: OCS-000003"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Kontoinhaber: Max Mustermann"));
    TEST_ASSERT_NOT_NULL(strstr(out, "IBAN: DE89 **** 3000"));
    TEST_ASSERT_NULL(strstr(out, "DE89370400440532013000"));
    TEST_ASSERT_NOT_NULL(strstr(out, "Ich ermächtige Carsharing Testverein e.V., Zahlungen"));
    TEST_ASSERT_EQUAL_INT(-1, mandate_mail_body(out, 20, "Erika", &m));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_normalize_iban_spaces_lowercase);
    RUN_TEST(test_iban_valid_vectors);
    RUN_TEST(test_bic_valid);
    RUN_TEST(test_creditor_id_valid);
    RUN_TEST(test_iban_mask);
    RUN_TEST(test_mandate_ref);
    RUN_TEST(test_mandate_text_fills_creditor);
    RUN_TEST(test_mandate_mail_body);
    return UNITY_END();
}
