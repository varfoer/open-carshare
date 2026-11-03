#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "core/invoice_doc.h"
#include "core/pdf.h"

void setUp(void) {}
void tearDown(void) {}

static void test_winansi_umlauts_euro(void) {
    char out[16];
    int n = pdf_winansi("Grüße €", out, sizeof out);
    TEST_ASSERT_EQUAL_INT(7, n);
    TEST_ASSERT_EQUAL_MEMORY("\x47\x72\xFC\xDF\x65\x20\x80", out, 7);
    pdf_winansi("a\xE2\x80\x9E" "b\xE2\x80\x9C" "c\xE2\x80\x93" "d\xE2\x88\x92" "e\xF0\x9F\x98\x80", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("a\x84" "b\x93" "c\x96" "d?e?", out);
    TEST_ASSERT_EQUAL_INT(-1, pdf_winansi("abcdef", out, 6));   /* needs 7 */
}

static void test_text_width_metrics(void) {
    TEST_ASSERT_EQUAL_INT(7, pdf_text_width(0, 10, "A"));       /* 667 -> 6.67 up */
    TEST_ASSERT_EQUAL_INT(6, pdf_text_width(0, 10, "a"));       /* 556 */
    TEST_ASSERT_EQUAL_INT(3, pdf_text_width(0, 10, " "));       /* 278 */
    TEST_ASSERT_EQUAL_INT(73, pdf_text_width(1, 100, "A"));     /* 722 -> 72.2 up */
    TEST_ASSERT_EQUAL_INT(56, pdf_text_width(1, 100, "a"));     /* 556 */
}

static char *mk(size_t *len) {
    pdf_t *p = pdf_new();
    pdf_page(p);
    pdf_text(p, 0, 10, 50, 700, "a (b) \\ c");
    pdf_hline(p, 50, 500, 690);
    pdf_page(p);
    pdf_text(p, 1, 12, 50, 700, "zwei");
    char *out = NULL;
    TEST_ASSERT_EQUAL_INT(0, pdf_finish(p, &out, len));
    return out;
}

static void test_pdf_xref_offsets(void) {
    size_t len;
    char *f = mk(&len);
    const char *sx = strstr(f, "startxref\n");
    TEST_ASSERT_NOT_NULL(sx);
    size_t xoff = (size_t)strtoul(sx + 10, NULL, 10);
    TEST_ASSERT_EQUAL_MEMORY("xref\n", f + xoff, 5);
    const char *q = f + xoff + 5;
    unsigned first, count;
    int used;
    TEST_ASSERT_EQUAL_INT(2, sscanf(q, "%u %u%n", &first, &count, &used));
    TEST_ASSERT_EQUAL_UINT(0, first);
    TEST_ASSERT_EQUAL_UINT(4 + 2 * 2 + 1, count);
    q = strchr(q, '\n') + 1;
    TEST_ASSERT_EQUAL_MEMORY("0000000000 65535 f \n", q, 20);
    for (unsigned n = 1; n < count; n++) {
        q += 20;
        size_t off = (size_t)strtoul(q, NULL, 10);
        TEST_ASSERT_EQUAL_INT(' ', q[10]);
        char want[32];
        int wl = snprintf(want, sizeof want, "%u 0 obj", n);
        TEST_ASSERT_EQUAL_MEMORY(want, f + off, (size_t)wl);
    }
    TEST_ASSERT_NOT_NULL(strstr(f, "/Count 2"));
    TEST_ASSERT_NOT_NULL(strstr(f, "(a \\(b\\) \\\\ c)"));
    free(f);
}

static void test_pdf_header_and_encoding(void) {
    size_t len;
    char *f = mk(&len);
    TEST_ASSERT_EQUAL_MEMORY("%PDF-1.4", f, 8);
    TEST_ASSERT_NOT_NULL(strstr(f, "/WinAnsiEncoding"));
    TEST_ASSERT_NOT_NULL(strstr(f, "/BaseFont /Helvetica-Bold"));
    TEST_ASSERT_TRUE(len > 6);
    TEST_ASSERT_EQUAL_MEMORY("%%EOF\n", f + len - 6, 6);
    free(f);
}

static void test_pdf_sticky_error(void) {
    char *out = NULL; size_t len = 0;
    pdf_t *p = pdf_new();
    pdf_text(p, 0, 10, 0, 0, "no page yet");
    pdf_page(p);
    TEST_ASSERT_EQUAL_INT(-1, pdf_finish(p, &out, &len));
    p = pdf_new();
    pdf_page(p);
    char big[600];
    memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    pdf_text(p, 0, 10, 0, 0, big);
    TEST_ASSERT_EQUAL_INT(-1, pdf_finish(p, &out, &len));
    TEST_ASSERT_EQUAL_INT(-1, pdf_finish(pdf_new(), &out, &len));   /* no page */
}

static settings_t S;
static invoice_t INV;

static void fill(void) {
    memset(&S, 0, sizeof S);
    memset(&INV, 0, sizeof INV);
    strcpy(S.assoc_name, "Carsharing Testverein e.V.");
    strcpy(S.assoc_street, "Hauptstr. 1");
    strcpy(S.assoc_zip, "12345");
    strcpy(S.assoc_city, "Musterstadt");
    strcpy(S.creditor_id, "DE98ZZZ09999999999");
    strcpy(S.invoice_footer, "Carsharing Testverein e.V. · Amtsgericht Musterstadt VR 1\nIBAN DE89 3704 0044 0532 0130 00");
    INV.number = 17; INV.total_cents = 5705;
    strcpy(INV.period, "2026-10");
    strcpy(INV.name, "Jürgen Müller"); strcpy(INV.street, "Gartenweg 3");
    strcpy(INV.zip, "54321"); strcpy(INV.city, "Beispieldorf");
    INV.issued_at = 1794312000;   /* 2026-11-10 */
}

static invoice_line_t mkline(int i) {
    invoice_line_t l;
    memset(&l, 0, sizeof l);
    l.invoice_number = 17; l.booking_id = i + 1;
    strcpy(l.car_name, "Zoe");
    l.start = 1791180000 + i * 3600; l.end = l.start + 4 * 3600 + 1800;   /* 2026-10-05 08:00 CEST */
    l.day_min = 270; l.km = 42; l.total_cents = 95;
    return l;
}

static void test_invoice_pdf_paginates(void) {
    fill();
    enum { N = 60 };
    static invoice_line_t lines[N];
    for (int i = 0; i < N; i++) { lines[i] = mkline(i); lines[i].cap_cut_cents = 150; }
    invoice_doc_t d = { &S, &INV, lines, N, "OCS-000012", "DE89 **** 3000", "2026-11-09" };
    char *out; size_t len;
    TEST_ASSERT_EQUAL_INT(0, invoice_pdf(&d, &out, &len));
    TEST_ASSERT_NOT_NULL(strstr(out, "/Count 3"));
    TEST_ASSERT_NOT_NULL(strstr(out, "(Rechnung 17 \\226 Seite 2)"));
    TEST_ASSERT_NOT_NULL(strstr(out, "(Deckelung -1,50 \\200)"));
    free(out);
    /* few lines: one page */
    d.n_lines = 3;
    TEST_ASSERT_EQUAL_INT(0, invoice_pdf(&d, &out, &len));
    TEST_ASSERT_NOT_NULL(strstr(out, "/Count 1"));
    free(out);
}

static void test_invoice_pdf_contains_amount(void) {
    fill();
    invoice_line_t l = mkline(0);
    invoice_doc_t d = { &S, &INV, &l, 1, "OCS-000012", "DE89 **** 3000", "2026-11-09" };
    char *out; size_t len;
    TEST_ASSERT_EQUAL_INT(0, invoice_pdf(&d, &out, &len));
    TEST_ASSERT_NOT_NULL(strstr(out, "(57,05 \\200)"));
    TEST_ASSERT_NOT_NULL(strstr(out, "(J\\374rgen M\\374ller)"));
    TEST_ASSERT_NOT_NULL(strstr(out, "(Datum: 10.11.2026)"));
    TEST_ASSERT_NOT_NULL(strstr(out, "(Zeitraum: Oktober 2026)"));
    TEST_ASSERT_NOT_NULL(strstr(out, "(05.10.)"));
    TEST_ASSERT_NOT_NULL(strstr(out, "(08:00\\22612:30)"));
    TEST_ASSERT_NOT_NULL(strstr(out, "(4,5)"));
    free(out);
    /* missing mandate data with total > 0 is an error, total 0 is fine without */
    d.mandate_ref = NULL;
    TEST_ASSERT_EQUAL_INT(-1, invoice_pdf(&d, &out, &len));
    INV.total_cents = 0;
    d.iban_masked = NULL; d.collection_date = NULL;
    TEST_ASSERT_EQUAL_INT(0, invoice_pdf(&d, &out, &len));
    TEST_ASSERT_NOT_NULL(strstr(out, "(Es wird kein Betrag eingezogen.)"));
    free(out);
}

static void test_mail_body_prenotification(void) {
    fill();
    invoice_doc_t d = { &S, &INV, NULL, 0, "OCS-000012", "DE89 **** 3000", "2026-11-09" };
    char out[1024];
    int n = invoice_mail_body(&d, "Jürgen", out, sizeof out);
    const char *want =
        "Hallo Jürgen,\n\nanbei deine Rechnung 17 für Oktober 2026.\n\n"
        "Wir ziehen den Gesamtbetrag von 57,05 € am 09.11.2026 per SEPA-Lastschrift von deinem Konto "
        "DE89 **** 3000 ein. Mandatsreferenz: OCS-000012, Gläubiger-ID: DE98ZZZ09999999999.\n\n"
        "Viele Grüße\nCarsharing Testverein e.V.\n";
    TEST_ASSERT_EQUAL_STRING(want, out);
    TEST_ASSERT_EQUAL_INT((int)strlen(want), n);
    TEST_ASSERT_EQUAL_INT(-1, invoice_mail_body(&d, "Jürgen", out, 50));
}

static void test_mail_body_zero(void) {
    fill();
    INV.total_cents = 0;
    invoice_doc_t d = { &S, &INV, NULL, 0, NULL, NULL, NULL };
    char out[512];
    TEST_ASSERT_TRUE(invoice_mail_body(&d, "Anna", out, sizeof out) > 0);
    TEST_ASSERT_EQUAL_STRING("Hallo Anna,\n\nanbei deine Rechnung 17 für Oktober 2026.\n\n"
                             "Es wird kein Betrag eingezogen.\n\nViele Grüße\nCarsharing Testverein e.V.\n", out);
}

static void test_money_de(void) {
    char o[32];
    money_de(123456, o, sizeof o); TEST_ASSERT_EQUAL_STRING("1.234,56 €", o);
    money_de(5, o, sizeof o); TEST_ASSERT_EQUAL_STRING("0,05 €", o);
    money_de(-5, o, sizeof o); TEST_ASSERT_EQUAL_STRING("-0,05 €", o);
    money_de(0, o, sizeof o); TEST_ASSERT_EQUAL_STRING("0,00 €", o);
    money_de(100000000, o, sizeof o); TEST_ASSERT_EQUAL_STRING("1.000.000,00 €", o);
    money_de(-99900, o, sizeof o); TEST_ASSERT_EQUAL_STRING("-999,00 €", o);
}

static void test_subject(void) {
    fill();
    char o[64];
    invoice_subject(&INV, o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("Rechnung 17 – Oktober 2026", o);
    strcpy(INV.period, "2027-03");
    invoice_subject(&INV, o, sizeof o);
    TEST_ASSERT_EQUAL_STRING("Rechnung 17 – März 2027", o);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_winansi_umlauts_euro);
    RUN_TEST(test_text_width_metrics);
    RUN_TEST(test_pdf_xref_offsets);
    RUN_TEST(test_pdf_header_and_encoding);
    RUN_TEST(test_pdf_sticky_error);
    RUN_TEST(test_invoice_pdf_paginates);
    RUN_TEST(test_invoice_pdf_contains_amount);
    RUN_TEST(test_mail_body_prenotification);
    RUN_TEST(test_mail_body_zero);
    RUN_TEST(test_money_de);
    RUN_TEST(test_subject);
    return UNITY_END();
}
