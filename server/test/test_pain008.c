#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "core/pain008.h"

void setUp(void) {}
void tearDown(void) {}

static void test_translit_umlauts_and_ampersand(void) {
    char out[80];
    sepa_translit("Jürgen Müller-Weiß & Söhne", out, sizeof out, 70);
    TEST_ASSERT_EQUAL_STRING("Juergen Mueller-Weiss + Soehne", out);
    sepa_translit("ÄÖÜ äöü Éé Çñ Ø€", out, sizeof out, 70);
    TEST_ASSERT_EQUAL_STRING("AeOeUe aeoeue Ee Cn O.", out);
    sepa_translit("  padded  ", out, sizeof out, 70);
    TEST_ASSERT_EQUAL_STRING("padded", out);
}

static void test_translit_truncates_70(void) {
    char in[200], out[200], small[5];
    memset(in, 'a', 100); in[100] = 0;
    sepa_translit(in, out, sizeof out, 70);
    TEST_ASSERT_EQUAL_size_t(70, strlen(out));
    sepa_translit(in, small, sizeof small, 70);
    TEST_ASSERT_EQUAL_STRING("aaaa", small);
}

static void test_translit_xml_specials(void) {
    char out[40];
    sepa_translit("a<b>c\"d", out, sizeof out, 70);
    TEST_ASSERT_EQUAL_STRING("a.b.c.d", out);
}

static void fill_hdr(pain008_hdr_t *h) {
    memset(h, 0, sizeof *h);
    strcpy(h->msg_id, "OCS-RUN-3");
    strcpy(h->created, "2026-11-03T10:15:00");
    strcpy(h->creditor_name, "Carsharing Musterstadt e.V.");
    strcpy(h->creditor_iban, "DE89370400440532013000");
    strcpy(h->creditor_id, "DE98ZZZ09999999999");
    strcpy(h->collection_date, "2026-11-09");
}

static void test_build_two_tx(void) {
    pain008_hdr_t h; pain008_tx_t tx[2];
    fill_hdr(&h);
    memset(tx, 0, sizeof tx);
    strcpy(tx[0].end_to_end, "OCS-1"); strcpy(tx[0].name, "Jürgen Müller-Weiß & Söhne");
    strcpy(tx[0].iban, "DE02120300000000202051"); strcpy(tx[0].bic, "BYLADEM1001");
    strcpy(tx[0].mandate_ref, "OCS-000012"); strcpy(tx[0].mandate_date, "2026-01-15");
    tx[0].amount_cents = 1234; strcpy(tx[0].remittance, "Rechnung 1 <Okt>");
    strcpy(tx[1].end_to_end, "OCS-2"); strcpy(tx[1].name, "Erika Mustermann");
    strcpy(tx[1].iban, "DE89370400440532013000");
    strcpy(tx[1].mandate_ref, "OCS-000013"); strcpy(tx[1].mandate_date, "2026-02-01");
    tx[1].amount_cents = 4471; strcpy(tx[1].remittance, "Rechnung 2");

    char *xml = NULL; size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, pain008_build(&h, tx, 2, &xml, &len));
    TEST_ASSERT_NOT_NULL(xml);
    TEST_ASSERT_EQUAL_size_t(strlen(xml), len);
    TEST_ASSERT_NOT_NULL(strstr(xml, "<NbOfTxs>2</NbOfTxs>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<CtrlSum>57.05</CtrlSum>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<SeqTp>RCUR</SeqTp>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<LclInstrm><Cd>CORE</Cd></LclInstrm>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<ReqdColltnDt>2026-11-09</ReqdColltnDt>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<MndtId>OCS-000012</MndtId>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<InstdAmt Ccy=\"EUR\">12.34</InstdAmt>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<InstdAmt Ccy=\"EUR\">44.71</InstdAmt>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<Nm>Juergen Mueller-Weiss + Soehne</Nm>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<Ustrd>Rechnung 1 .Okt.</Ustrd>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "<BIC>BYLADEM1001</BIC>"));
    TEST_ASSERT_NOT_NULL(strstr(xml, "NOTPROVIDED"));   /* creditor and tx 2 have no BIC */
    TEST_ASSERT_NULL(strstr(xml, "&amp;"));

    const char *dir = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/ocs-pain008-test.xml", dir && *dir ? dir : "/tmp");
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(len, fwrite(xml, 1, len, f));
    fclose(f);
    free(xml);
}

static void test_build_rejects_bad_input(void) {
    pain008_hdr_t h; pain008_tx_t tx;
    fill_hdr(&h);
    memset(&tx, 0, sizeof tx);
    char *xml = NULL; size_t len = 0;
    TEST_ASSERT_EQUAL_INT(-1, pain008_build(&h, &tx, 0, &xml, &len));
    TEST_ASSERT_EQUAL_INT(-1, pain008_build(&h, &tx, 1, &xml, &len));   /* amount 0 */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_translit_umlauts_and_ampersand);
    RUN_TEST(test_translit_truncates_70);
    RUN_TEST(test_translit_xml_specials);
    RUN_TEST(test_build_two_tx);
    RUN_TEST(test_build_rejects_bad_input);
    return UNITY_END();
}
