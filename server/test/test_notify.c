#include <string.h>

#include "core/notify.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define S0 1793799000 /* 2026-11-04 13:30 UTC = 14:30 Berlin */
#define BASE "https://carshare.test"

static const car_t CAR = {.id = 1, .name = "Zoe", .plate = "KI-CS 1", .location = "Hof Mitte"};

static booking_t bk(void) {
  booking_t b = {.id = 42, .car_id = 1, .user_id = 3, .booked_start = S0, .booked_end = S0 + 3 * 3600};
  return b;
}

static notify_booking_t nb(notify_kind_t k, const booking_t *b) {
  notify_booking_t n = {.kind = k, .b = b, .car = &CAR, .user_name = "Anna", .base_url = BASE};
  return n;
}

static char subj[256], body[2048];

void test_local_time_cet(void) {
  char t[17];
  notify_local_time(1762264800, t);
  TEST_ASSERT_EQUAL_STRING("04.11.2025 15:00", t);
}

void test_local_time_cest(void) {
  char t[17];
  notify_local_time(1782900000, t); /* 2026-07-01 10:00 UTC */
  TEST_ASSERT_EQUAL_STRING("01.07.2026 12:00", t);
}

void test_created_golden(void) {
  booking_t b = bk();
  notify_booking_t n = nb(NT_CREATED, &b);
  TEST_ASSERT_EQUAL_INT(0, notify_booking_mail(&n, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_EQUAL_STRING("Buchung 42 bestätigt", subj);
  TEST_ASSERT_EQUAL_STRING("Hallo Anna,\n\n"
                           "deine Buchung ist eingetragen:\n\n"
                           "Auto: Zoe (KI-CS 1)\n"
                           "Von: 04.11.2026 14:30\n"
                           "Bis: 04.11.2026 17:30\n"
                           "Standort: Hof Mitte\n\n"
                           "Meine Buchungen: https://carshare.test/#/buchungen\n",
                           body);
}

void test_modified_by_admin_golden(void) {
  booking_t b = bk();
  b.club_trip = true;
  notify_booking_t n = nb(NT_MODIFIED, &b);
  n.by_admin = true;
  TEST_ASSERT_EQUAL_INT(0, notify_booking_mail(&n, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_EQUAL_STRING("Buchung 42 geändert", subj);
  TEST_ASSERT_EQUAL_STRING("Hallo Anna,\n\n"
                           "deine Buchung wurde geändert: Die Änderung hat die Vereinsverwaltung vorgenommen.\n\n"
                           "Auto: Zoe (KI-CS 1)\n"
                           "Von: 04.11.2026 14:30\n"
                           "Bis: 04.11.2026 17:30\n"
                           "Vereinsfahrt\n"
                           "Standort: Hof Mitte\n\n"
                           "Meine Buchungen: https://carshare.test/#/buchungen\n",
                           body);
}

void test_cancelled_with_fee_golden(void) {
  booking_t b = bk();
  b.cancel_fee_cents = 500;
  notify_booking_t n = nb(NT_CANCELLED, &b);
  TEST_ASSERT_EQUAL_INT(0, notify_booking_mail(&n, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_EQUAL_STRING("Buchung 42 storniert", subj);
  TEST_ASSERT_EQUAL_STRING("Hallo Anna,\n\n"
                           "deine Buchung wurde storniert:\n\n"
                           "Auto: Zoe (KI-CS 1)\n"
                           "Von: 04.11.2026 14:30\n"
                           "Bis: 04.11.2026 17:30\n"
                           "Stornogebühr: 5,00 €\n\n"
                           "Meine Buchungen: https://carshare.test/#/buchungen\n",
                           body);
}

void test_remind_start_golden(void) {
  booking_t b = bk();
  notify_booking_t n = nb(NT_REMIND_START, &b);
  n.start_tolerance_min = 15;
  TEST_ASSERT_EQUAL_INT(0, notify_booking_mail(&n, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_EQUAL_STRING("Erinnerung: Fahrt ab 14:30", subj);
  TEST_ASSERT_EQUAL_STRING("Hallo Anna,\n\n"
                           "deine Fahrt beginnt bald:\n\n"
                           "Auto: Zoe (KI-CS 1)\n"
                           "Von: 04.11.2026 14:30\n"
                           "Bis: 04.11.2026 17:30\n"
                           "Standort: Hof Mitte\n"
                           "Starten kannst du ab 04.11.2026 14:15.\n\n"
                           "Meine Buchungen: https://carshare.test/#/buchungen\n",
                           body);
}

void test_remind_late_fee_golden(void) {
  booking_t b = bk();
  notify_booking_t n = nb(NT_REMIND_LATE, &b);
  n.start_tolerance_min = 15;
  n.late_fee_cents_per_h = 300;
  TEST_ASSERT_EQUAL_INT(0, notify_booking_mail(&n, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_EQUAL_STRING("Rückgabe überfällig: Zoe", subj);
  TEST_ASSERT_EQUAL_STRING("Hallo Anna,\n\n"
                           "deine Buchung ist am 04.11.2026 17:30 abgelaufen, die Fahrt ist aber noch nicht "
                           "beendet. Bitte bring das Auto zurück und beende die Fahrt unter „Meine Buchungen“.\n\n"
                           "Auto: Zoe (KI-CS 1)\n"
                           "Von: 04.11.2026 14:30\n"
                           "Bis: 04.11.2026 17:30\n"
                           "Standort: Hof Mitte\n"
                           "Ab 04.11.2026 17:45 kostet jede angefangene Stunde 3,00 € Verspätungsgebühr.\n\n"
                           "Meine Buchungen: https://carshare.test/#/buchungen\n",
                           body);
}

void test_remind_late_club_no_fee_line(void) {
  booking_t b = bk();
  b.club_trip = true;
  notify_booking_t n = nb(NT_REMIND_LATE, &b);
  n.start_tolerance_min = 15;
  n.late_fee_cents_per_h = 300;
  TEST_ASSERT_EQUAL_INT(0, notify_booking_mail(&n, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_NOT_NULL(strstr(body, "Vereinsfahrt\n"));
  TEST_ASSERT_NULL(strstr(body, "Verspätungsgebühr"));
  b.club_trip = false;
  n.late_fee_cents_per_h = 0;
  TEST_ASSERT_EQUAL_INT(0, notify_booking_mail(&n, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_NULL(strstr(body, "Verspätungsgebühr"));
}

void test_remind_end_golden(void) {
  booking_t b = bk();
  notify_booking_t n = nb(NT_REMIND_END, &b);
  n.start_tolerance_min = 15;
  n.late_fee_cents_per_h = 300;
  TEST_ASSERT_EQUAL_INT(0, notify_booking_mail(&n, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_EQUAL_STRING("Erinnerung: Rückgabe bis 17:30", subj);
  TEST_ASSERT_EQUAL_STRING("Hallo Anna,\n\n"
                           "deine Buchung endet bald. Bitte bring das Auto rechtzeitig zurück und beende die "
                           "Fahrt unter „Meine Buchungen“. Brauchst du länger, verlängere die Buchung dort, "
                           "solange das Auto frei ist.\n\n"
                           "Auto: Zoe (KI-CS 1)\n"
                           "Von: 04.11.2026 14:30\n"
                           "Bis: 04.11.2026 17:30\n"
                           "Standort: Hof Mitte\n"
                           "Ab 04.11.2026 17:45 kostet jede angefangene Stunde 3,00 € Verspätungsgebühr.\n\n"
                           "Meine Buchungen: https://carshare.test/#/buchungen\n",
                           body);
}

void test_push_texts(void) {
  booking_t b = bk();
  char t[128], x[512];
  notify_booking_t n = nb(NT_REMIND_START, &b);
  n.start_tolerance_min = 15;
  TEST_ASSERT_EQUAL_INT(0, notify_booking_push(&n, t, sizeof t, x, sizeof x));
  TEST_ASSERT_EQUAL_STRING("Fahrt ab 14:30", t);
  TEST_ASSERT_EQUAL_STRING("Zoe: Starten kannst du ab 14:15.", x);
  n.kind = NT_REMIND_END;
  TEST_ASSERT_EQUAL_INT(0, notify_booking_push(&n, t, sizeof t, x, sizeof x));
  TEST_ASSERT_EQUAL_STRING("Rückgabe bis 17:30", t);
  TEST_ASSERT_EQUAL_STRING("Zoe: Bitte rechtzeitig zurückbringen oder die Buchung verlängern.", x);
  n.kind = NT_REMIND_LATE;
  TEST_ASSERT_EQUAL_INT(0, notify_booking_push(&n, t, sizeof t, x, sizeof x));
  TEST_ASSERT_EQUAL_STRING("Rückgabe überfällig: Zoe", t);
  TEST_ASSERT_EQUAL_STRING("Die Buchung ist um 17:30 abgelaufen. Bitte bring das Auto zurück und beende die Fahrt.", x);
  n.kind = NT_CREATED;
  TEST_ASSERT_EQUAL_INT(-1, notify_booking_push(&n, t, sizeof t, x, sizeof x));
  n.kind = NT_REMIND_END;
  TEST_ASSERT_EQUAL_INT(-1, notify_booking_push(&n, t, 10, x, sizeof x));
}

static notify_run_t run(const int64_t *failed, size_t n_failed) {
  notify_run_t r = {.run_id = 3, .period = "2026-10", .n_invoices = 4, .n_mailed_total = 2,
                    .n_sent_now = 2, .failed = failed, .n_failed = n_failed, .total_cents = 123456,
                    .collection_date = "2026-11-10", .base_url = BASE};
  return r;
}

void test_run_mail_failed_golden(void) {
  int64_t f[] = {17, 18};
  notify_run_t r = run(f, 2);
  TEST_ASSERT_EQUAL_INT(0, notify_run_mail(&r, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_EQUAL_STRING("Abrechnung Oktober 2026 versendet", subj);
  TEST_ASSERT_EQUAL_STRING("Abrechnungslauf 3 (Oktober 2026)\n\n"
                           "Rechnungen versendet: 2 von 4\n"
                           "Bei diesem Versand: 2\n"
                           "Fehlgeschlagen: 17, 18\n"
                           "Summe: 1.234,56 €\n"
                           "Einzugstermin: 10.11.2026\n\n"
                           "Bitte den Versand für die fehlgeschlagenen Rechnungen wiederholen.\n\n"
                           "https://carshare.test/#/admin/abrechnung\n",
                           body);
}

void test_run_mail_no_failed(void) {
  notify_run_t r = run(NULL, 0);
  r.n_mailed_total = 4;
  TEST_ASSERT_EQUAL_INT(0, notify_run_mail(&r, subj, sizeof subj, body, sizeof body));
  TEST_ASSERT_EQUAL_STRING("Abrechnungslauf 3 (Oktober 2026)\n\n"
                           "Rechnungen versendet: 4 von 4\n"
                           "Bei diesem Versand: 2\n"
                           "Fehlgeschlagen: keine\n"
                           "Summe: 1.234,56 €\n"
                           "Einzugstermin: 10.11.2026\n\n"
                           "https://carshare.test/#/admin/abrechnung\n",
                           body);
}

void test_too_small_buffer_returns_minus1(void) {
  booking_t b = bk();
  notify_booking_t n = nb(NT_CREATED, &b);
  char small[40];
  TEST_ASSERT_EQUAL_INT(-1, notify_booking_mail(&n, subj, sizeof subj, small, sizeof small));
  TEST_ASSERT_EQUAL_INT(-1, notify_booking_mail(&n, small, 5, body, sizeof body));
  int64_t f[] = {17};
  notify_run_t r = run(f, 1);
  TEST_ASSERT_EQUAL_INT(-1, notify_run_mail(&r, subj, sizeof subj, small, sizeof small));
  TEST_ASSERT_EQUAL_INT(-1, notify_run_mail(&r, small, 5, body, sizeof body));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_local_time_cet);
  RUN_TEST(test_local_time_cest);
  RUN_TEST(test_created_golden);
  RUN_TEST(test_modified_by_admin_golden);
  RUN_TEST(test_cancelled_with_fee_golden);
  RUN_TEST(test_remind_start_golden);
  RUN_TEST(test_remind_late_fee_golden);
  RUN_TEST(test_remind_late_club_no_fee_line);
  RUN_TEST(test_remind_end_golden);
  RUN_TEST(test_push_texts);
  RUN_TEST(test_run_mail_failed_golden);
  RUN_TEST(test_run_mail_no_failed);
  RUN_TEST(test_too_small_buffer_returns_minus1);
  return UNITY_END();
}
