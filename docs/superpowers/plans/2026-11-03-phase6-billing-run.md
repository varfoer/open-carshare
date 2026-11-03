# Phase 6: Billing Run Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The admin bills a month end-to-end: draft with live recompute, finalize (gapless invoice numbers, bookings locked), send (invoice PDF mails that are the SEPA pre-notification) and a `pain.008.001.02` file for Hibiscus.

**Architecture:** Pure pieces in `core/`: period/TARGET2 calendar and line building (`billing.c`), a minimal PDF writer (`pdf.c`), the invoice layout and pre-notification text (`invoice_doc.c`), the XML writer with SEPA transliteration (`pain008.c`) and a multipart mail builder (`mail_build.c`). Three new db tables (`billing_runs`, `invoices`, `invoice_lines`) plus a batch finalize write. `http/api_billing.c` serves the admin endpoints; a draft is never persisted beyond its run row: `GET` computes it live from the bookings, so "recompute" is a reload.

**Tech Stack:** unchanged: C11, CMake, Mongoose 7.14, cJSON, Unity, vanilla JS (ES2020), `node --test`. Optional test tool: `xmllint` (libxml2-utils) for the XSD check.

**Spec:** `docs/spec.md` §4 (booking, invoice, billing_run, settings), §6, §9, §13, §15 Phase 6; decisions below are binding and go into the spec in Task 10.

**Branch:** `phase6-billing-run`.

**Out of scope:** "billing run done" admin mail and all other §10 mails (Phase 7); invoices visible to users in the portal; excluding a user from a run; credit notes / cancelling a final invoice.

## Product-owner decision (2026-11-03)

- Pre-notification period is **5 calendar days**, agreed in the mandate text. Nothing is released and there are no users, so the text changes in place: `MANDATE_TEXT_VERSION` becomes 2, no migration.

## Builder calls (not asked; change in review if wrong)

1. **Collection date is fixed at send, not finalize.** The send mail is the pre-notification, so the date is anchored there: `collection_date` = first TARGET2 business day on or after (local send date + `sepa_lead_days`). The SEPA XML and the PDFs are generated at the first send (the PDF carries the pre-notification block). Finalize assigns numbers and locks bookings only. Spec §9 steps 3/4 are reworded accordingly.
2. **`sepa_lead_days`** setting: integer 5..30, default 5 (`SEPA_PRENOTIFY_MIN_DAYS 5` in `sepa.h`: the mandate text promises 5, so a smaller value is invalid).
3. **Mandate text v2** also names the creditor address (backlog item "check with the bank whether the text must show the address": included now, cheaper than a later bump). Mandates need `assoc_street`, `assoc_zip`, `assoc_city` set in addition to name and creditor ID.
4. **Non-EEA IBANs need a BIC** (backlog P6): `POST /api/me/mandate` with an IBAN of `AD CH GB GI MC SM VA` and no BIC is 400 `bic_required` „Für diese IBAN ist die BIC erforderlich.“.
5. **Inclusion:** a booking is billed in the first run whose period end is after its bill time, not only in "its" month: `invoice_id == 0` and bill time `< period_end` (bill time = `actual_end`, or `booked_end` if never started, or `cancelled_at` for cancelled). So a booking whose times an admin moves into an already billed month lands in the next run instead of being lost. Cancelled bookings appear only with `cancel_fee_cents > 0` and not `club_trip`.
6. **Runs are ascending:** a new run needs a closed period (`period_end <= now`), no open draft, and a period greater than every existing run's period. A draft can be deleted.
7. **Invoice number** = global gapless integer starting at 1, shown as plain number („Rechnung 17“). It is also the invoice key: `booking.invoice_id` holds it.
8. **Invoice snapshot:** invoice rows keep addressee name/address (at finalize) and each line keeps its computed figures (`invoice_lines.jsonl`); PDFs are stored under `data/invoices/<number>.pdf`, the XML under `data/billing/run-<id>-pain008.xml`. Later tariff or user edits never change an issued invoice.
9. **Mandate used is the one current at send** (stored as `invoice.mandate_id`); finalize and send both refuse while a user with total > 0 has none.
10. **Send is retryable:** it mails only invoices with `mailed_at == 0`; collection date, PDFs and XML from the first send stay.
11. **Invoice date** = local date of the first send. PDF and mail address the user with „du“ like the other mails.
12. **Creditor BIC** optional: XML writes `<Othr><Id>NOTPROVIDED</Id></Othr>` when empty (same for debtor without BIC).
13. Admin booking edits (`PUT`/`DELETE /api/admin/bookings/{id}`) on a booking with `invoice_id != 0`: 409 `invoiced` „Die Buchung ist bereits abgerechnet.“.

## Global Constraints

- All Phase 1–5 Global Constraints still hold (`int64_t` UTC seconds, money `int64_t` cents, no float, German UI, English code, fixed `char[]` reject-not-truncate, `-Wall -Wextra -Werror`, ASan/UBSan tests, db copy-before-modify, every changed web file in `web/sw.js` `SHELL` and `CACHE` bumped once per task that changes web files).
- Build/test from repo root: `cmake --build build && ctest --test-dir build --output-on-failure`; web: `node --test web/test/`.
- Period string `"YYYY-MM"`, year 2000..2099; bounds are local (Europe/Berlin) month starts via `berlin.h`.
- Prices only via `price_booking()` (`core/pricing.h`); invoice total = sum of line totals.
- SEPA: `pain.008.001.02`, `CORE`, `SeqTp` always `RCUR`, `ChrgBr` `SLEV`, `BtchBookg` `true`. Text fields restricted to the SEPA set `A-Z a-z 0-9 / - ? : ( ) . , ' +` and space; names max 70, `Ustrd` max 140.
- TARGET2 closing days: Saturday, Sunday, 1 Jan, Good Friday, Easter Monday, 1 May, 25 Dec, 26 Dec.
- Full IBANs never in API responses or mails (`iban_masked` only); they appear only in the XML file.
- Nothing is deployed: no compatibility with older data files needed; missing new files = empty tables.

## Review Focus

1. **Booking edited between viewing the draft and finalize:** the invoice must show the state at finalize, not what the admin saw. Test: Task 7 `test_finalize_uses_current_state`.
2. **Crash in the middle of finalize** must not burn invoice numbers or leave bookings locked to non-existent invoices. Test: Task 3 `test_load_drops_invoices_of_draft_run`.
3. **Send retried after some mails failed:** no second mail to those already sent, same collection date, same XML. Test: Task 8 `test_send_retry_mails_only_unmailed`.
4. **Collection date on a weekend or Easter Monday** moves to the next TARGET2 day. Test: Task 2 `test_collection_date_skips_easter_monday`.
5. **Holder „Jürgen Müller-Weiß & Söhne“** in the XML becomes `Juergen Mueller-Weiss + Soehne` (valid SEPA set, no XML break). Test: Task 5 `test_translit_umlauts_and_ampersand`.

---

## File Structure

```
server/src/core/sepa.{h,c}           mandate text v2, iban_needs_bic, SEPA_PRENOTIFY_MIN_DAYS
server/src/core/billing.{h,c}        NEW: period, TARGET2, collection date, inclusion, invoice line from price
server/src/core/model.h              booking.invoice_id; billing_run_t, invoice_t, invoice_line_t; settings fields
server/src/core/codec.{h,c}          codecs for the new types and fields
server/src/store/db.{h,c}            3 tables, db_finalize_run, db_put_invoices, load fixup
server/src/core/pain008.{h,c}        NEW: XML writer, transliteration
server/src/core/pdf.{h,c}            NEW: minimal PDF writer, Helvetica metrics, WinAnsi
server/src/core/invoice_doc.{h,c}    NEW: invoice PDF layout, pre-notification/mail text
server/src/core/mail_build.{h,c}     mail_att_t, multipart build
server/src/app.h, main.c, http/api.c mail_send_fn with attachment
server/src/http/api_billing.c        NEW: billing endpoints
server/src/http/api_admin.c          settings keys, invoiced lock
server/src/http/api_mandate.c        bic_required, address readiness
server/test/test_billing.c, test_pain008.c, test_pdf.c, test_api_billing.c   NEW
server/test/data/pain.008.001.02.xsd NEW (vendored, for the xmllint test)
web/js/billing.js, web/js/views/admin_billing.js, web/test/billing.test.mjs  NEW
web/js/views/admin_settings.js, admin_bookings.js, account.js, app.js/admin_ui.js, web/sw.js
docs/spec.md, docs/backlog.md, deploy/vps/README.md
```

---

### Task 1: Mandate text v2, BIC for non-EEA IBANs

**Files:** Modify `server/src/core/sepa.{h,c}`, `server/src/http/api_mandate.c`, `server/test/test_sepa.c`, `server/test/test_api_mandate.c`, `server/test/helpers.c` (`th_set_creditor` also sets street "Hauptstr. 1", zip "12345", city "Teststadt"; `th_add_user` mandate uses `MANDATE_TEXT_VERSION`), `web/js/views/account.js` only if it hardcodes texts.

**Interfaces (produces, `core/sepa.h`):**
```c
#define MANDATE_TEXT_VERSION 2
#define SEPA_PRENOTIFY_MIN_DAYS 5
bool iban_needs_bic(const char *iban);  /* normalized; true for AD CH GB GI MC SM VA */
int  mandate_text(char *out, size_t cap, const settings_t *s); /* uses assoc_name, address, creditor_id */
```
`mandate_mail_body` keeps its signature but takes the address from a `settings_t` too: `int mandate_mail_body(char *out, size_t cap, const char *user_name, const mandate_t *m, const settings_t *s)` (name and ID still from the mandate snapshot).

Golden text v2 (exact):
```
SEPA-Lastschriftmandat

Zahlungsempfänger: %name, %street, %zip %city
Gläubiger-Identifikationsnummer: %creditor_id
Mandatsreferenz: wird nach Erteilung des Mandats mitgeteilt

Ich ermächtige %name, Zahlungen von meinem Konto mittels Lastschrift einzuziehen. Zugleich weise ich mein Kreditinstitut an, die von %name auf mein Konto gezogenen Lastschriften einzulösen.

Der Zahlungsempfänger kündigt mir Betrag und Fälligkeitstag jeder Lastschrift spätestens 5 Kalendertage vor dem Fälligkeitstag an.

Hinweis: Ich kann innerhalb von acht Wochen, beginnend mit dem Belastungsdatum, die Erstattung des belasteten Betrages verlangen. Es gelten dabei die mit meinem Kreditinstitut vereinbarten Bedingungen.

Das Mandat gilt für wiederkehrende Lastschriften.
```

- [ ] **Step 1: Tests:** update `test_mandate_text_fills_creditor` golden to v2; `test_iban_needs_bic` (`CH9300762011623852957` true, `GB29NWBK60161331926819` true, `DE89370400440532013000` false, `AT611904300234573201` false); API: `test_post_mandate_ch_without_bic_400` (400 `bic_required`, nothing stored), `test_post_mandate_ch_with_bic_201`, `test_mandate_not_ready_without_address` (GET returns `ready:false`/creditor null as today when `assoc_city` is empty; POST 409 `no_creditor`).
- [ ] **Step 2:** run, expect FAIL.
- [ ] **Step 3:** implement; readiness = name, street, zip, city and creditor_id non-empty.
- [ ] **Step 4:** full `ctest` passes (other tests use `th_set_creditor`).
- [ ] **Step 5:** commit `feat(sepa): mandate text v2 with 5-day pre-notification, BIC for non-EEA IBANs`.

---

### Task 2: Pure billing calendar and lines

**Files:** Create `server/src/core/billing.{h,c}`, `server/test/test_billing.c`; Modify `server/CMakeLists.txt`, `server/src/core/model.h` (only `booking_t.invoice_id` and `invoice_line_t`, needed here; Task 3 adds the rest).

**Interfaces (produces):**
```c
/* model.h */
/* booking_t gains: int64_t invoice_id;  0 = not invoiced */
typedef struct {
  int64_t invoice_number, booking_id;
  char car_name[64];
  int64_t start, end;                 /* actual if started, else booked; cancelled: booked */
  bool never_started, cancelled, club_trip;
  int64_t day_min, night_min, unused_min, km;
  int64_t time_cents, km_cents, cap_cut_cents;   /* cap_cut = time_cap_cut + combined_cap_cut */
  int64_t late_fee_cents, cancel_fee_cents, total_cents;
} invoice_line_t;

/* billing.h */
int  billing_period_parse(const char *s, int *year, int *month); /* "YYYY-MM", 2000..2099, 01..12; 0 ok, -1 */
void billing_period_bounds(int year, int month, int64_t *from, int64_t *to); /* UTC of local month starts */
int64_t billing_bill_time(const booking_t *b);   /* DONE: actual_end or booked_end; CANCELLED: cancelled_at; else 0 */
/* invoice_id == 0, bill time in (0, period_end), and DONE, or CANCELLED with cancel_fee_cents > 0 && !club_trip. */
bool billing_includes(const booking_t *b, int64_t period_end);
void billing_line(const booking_t *b, const char *car_name, const price_t *p, invoice_line_t *out); /* invoice_number 0 */
bool target2_open(int year, int month, int day);
/* Local date of now + lead_days, moved forward to a TARGET2 day; writes "YYYY-MM-DD". */
void billing_collection_date(int64_t now, int lead_days, char out[11]);
void billing_local_date(int64_t utc, char out[11]);   /* "YYYY-MM-DD", Berlin */
```
Easter via the anonymous Gregorian algorithm (Meeus/Jones/Butcher).

- [ ] **Step 1: Tests (`test_billing.c`):**
  - `test_period_parse`: `"2026-10"` ok; `"2026-13"`, `"26-10"`, `"2026-1"`, `"2026-10x"`, `"1999-12"` → -1.
  - `test_period_bounds_local`: 2026-10 → from `1790805600` (2026-09-30T22:00Z), to `1793487600` (2026-10-31T23:00Z, after DST end).
  - `test_includes_month_end_local`: DONE booking `actual_end` 2026-10-31 23:30 local (`1793485800`, 22:30Z) is in 2026-10 (`< to`); `actual_end == 1793487600` is not.
  - `test_includes_rules`: invoiced (`invoice_id 5`) false; RESERVED/ACTIVE false; CANCELLED fee 0 false; CANCELLED fee 300 true; CANCELLED fee 300 club_trip false; never started DONE uses `booked_end`; earlier month (straggler) true.
  - `test_target2_days`: 2027-03-26 (Good Friday) and 2027-03-29 (Easter Monday) closed, 2027-03-30 open; 2026-05-01, 2026-12-25, 2026-12-26, 2027-01-01, Saturdays/Sundays closed.
  - `test_collection_date_skips_easter_monday`: now `1805878800` (2027-03-24 10:00 local), lead 5 → 2027-03-29 is Easter Monday → `"2027-03-30"`.
  - `test_collection_date_local_midnight`: now `1793835000` (2026-11-04 23:30Z = 2026-11-05 00:30 local), lead 5 → local 11-05 + 5 = `"2026-11-10"` (the UTC date would give `"2026-11-09"`); now `1793658600` (2026-11-02 23:30 local), lead 5 → 11-07 is Saturday → `"2026-11-09"`.
  - `test_line_from_price`: fills every field from a hand-made `price_t`; unused_min = unused_day + unused_night; never started → start/end booked.
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS.
- [ ] **Step 5:** commit `feat(core): billing period, TARGET2 calendar, invoice lines`.

---

### Task 3: Model, codecs, db tables, settings

**Files:** Modify `core/model.h`, `core/codec.{h,c}`, `store/db.{h,c}`, `test_codec.c`, `test_db.c`.

**Interfaces (produces):**
```c
typedef enum { RUN_DRAFT = 0, RUN_FINAL } run_state_t;
typedef struct {
  int64_t id, created_at, finalized_at, sent_at;   /* 0 = not yet */
  char period[8]; run_state_t state;
  char collection_date[11];                        /* "" until first send */
  char sepa_file[64];                              /* relative to data dir; "" none */
} billing_run_t;
typedef struct {
  int64_t number, user_id, run_id, total_cents, n_lines;
  int64_t created_at, issued_at, mailed_at, mandate_id;  /* issued/mailed/mandate 0 until send */
  char period[8];
  char name[96], street[96], zip[16], city[64];          /* addressee at finalize */
} invoice_t;
/* settings_t gains: int sepa_lead_days (default 5); char invoice_footer[512]; */
```
Codecs: `billing_run_to_json/from_json`, `invoice_to_json/from_json`, `invoice_line_to_json/from_json` (all keys required); `booking.invoice_id` optional on load (absent → 0); settings `sepa_lead_days` missing → 5, `invoice_footer` missing → "".

db (`db_t` gains `runs`, `invoices`, `lines` arrays):
```c
billing_run_t *db_run(db_t *db, int64_t id);
invoice_t *db_invoice(db_t *db, int64_t number);
int db_put_run(db_t *db, billing_run_t *r);          /* id==0: assign max+1 */
int db_delete_run(db_t *db, int64_t id);             /* -1 unknown or persist failed */
int db_put_invoices(db_t *db, const invoice_t *inv, size_t n);  /* replace by number, one write */
/* Inserts invoices and lines, sets invoice_id = line.invoice_number on every line's booking, then
 * replaces the run. Writes invoice_lines.jsonl, invoices.jsonl, bookings.jsonl, billing_runs.jsonl in
 * this order. RAM restored on -1. */
int db_finalize_run(db_t *db, const billing_run_t *run, const invoice_t *inv, size_t n_inv,
                    const invoice_line_t *lines, size_t n_lines);
int64_t db_next_invoice_number(const db_t *db);      /* max number + 1 */
/* Directory helpers: "<dir>/invoices/<number>.pdf", "<dir>/billing/run-<id>-pain008.xml"; -1 if it does not fit. */
int db_invoice_pdf_path(const db_t *db, int64_t number, char *out, size_t cap);
int db_run_xml_relpath(int64_t run_id, char *out, size_t cap);
```
`db_load` creates `invoices/` and `billing/`, and after loading drops (RAM only) invoices and lines whose run is not `RUN_FINAL` or missing, then sets `invoice_id = 0` on bookings whose invoice no longer exists.

- [ ] **Step 1: Tests:** codec round trips for the three types, booking with/without `invoice_id`, settings defaults; db: `test_finalize_run_persists_and_locks` (reload sees invoices, lines, run FINAL, bookings' `invoice_id`), `test_load_drops_invoices_of_draft_run` (write invoices + lines + bookings via `db_finalize_run`, then rewrite `billing_runs.jsonl` with the run as draft, reload: no invoices, no lines, bookings `invoice_id == 0`, `db_next_invoice_number == 1`), `test_next_invoice_number`.
- [ ] **Step 2–4:** FAIL, implement, PASS.
- [ ] **Step 5:** commit `feat(store): billing runs, invoices, invoice lines`.

---

### Task 4: Mail attachments

**Files:** Modify `core/mail_build.{h,c}`, `app.h`, `main.c`, `http/api.c`, `http/api.h`, `test/helpers.{h,c}`, `test_mail_build.c`, every `mail_send` caller that builds `app_t` by hand (fuzz harness).

**Interfaces (produces):**
```c
typedef struct { const char *filename, *content_type; const void *data; size_t len; } mail_att_t;
/* att NULL: identical output to today's text/plain message. Otherwise multipart/mixed with boundary
 * "=_ocs_part_0": text part as today, attachment base64 (76-char lines, CRLF),
 * Content-Disposition: attachment; filename="<filename>" (ASCII only, -1 otherwise). */
int mail_build_att(char *buf, size_t n, const char *from, const char *to, const char *subject,
                   const char *body, int64_t date, const mail_att_t *att);
size_t mail_size_bound(size_t body_len, size_t att_len);  /* enough for mail_build_att */
/* app.h */
typedef int (*mail_send_fn)(void *ctx, const char *to, const char *subject, const char *body,
                            const mail_att_t *att);
/* api.h */
int api_send_mail_att(app_t *a, const char *to, const char *subject, const char *body, const mail_att_t *att);
/* helpers.h */
const char *th_last_mail_att_name(void);   /* "" if none */
size_t th_last_mail_att_len(void);
```
`mail_build` becomes `mail_build_att(..., NULL)`; `main.c` sizes its buffer with `mail_size_bound` instead of `MAIL_BUF`.

- [ ] **Step 1: Tests:** `test_att_null_same_as_plain` (byte-identical to the current output), `test_att_multipart` (headers `Content-Type: multipart/mixed; boundary="=_ocs_part_0"`, base64 of `"%PDF-1.4\n"` = `JVBERi0xLjQK`, closing `--=_ocs_part_0--`), `test_att_filename_non_ascii_rejected`, `test_size_bound_suffices` (1 MiB attachment).
- [ ] **Step 2–4.** **Step 5:** commit `feat(mail): attachments`.

---

### Task 5: pain.008.001.02 writer

**Files:** Create `core/pain008.{h,c}`, `test/test_pain008.c`, `test/data/pain.008.001.02.xsd`; Modify `CMakeLists.txt` (test, plus an `add_test(pain008_xsd ...)` that runs `xmllint --noout --schema` on the file written by `test_pain008` and is skipped via `SKIP_RETURN_CODE` when `xmllint` or the XSD is missing).

**Interfaces (produces):**
```c
/* UTF-8 to SEPA set: ä ae, ö oe, ü ue, Ä Ae, Ö Oe, Ü Ue, ß ss, & +, accented Latin-1 letters to base
 * letter, anything else outside the set to '.'; trimmed; at most max_chars. Always NUL-terminates. */
void sepa_translit(const char *utf8, char *out, size_t cap, size_t max_chars);
typedef struct {
  char msg_id[36], created[20];          /* created "YYYY-MM-DDThh:mm:ss" UTC */
  char creditor_name[71], creditor_iban[35], creditor_bic[12], creditor_id[36];
  char collection_date[11];
} pain008_hdr_t;
typedef struct {
  char end_to_end[36], name[71], iban[35], bic[12], mandate_ref[36], mandate_date[11];
  int64_t amount_cents; char remittance[141];
} pain008_tx_t;
/* One PmtInf (CORE, RCUR). Names/remittance pass through sepa_translit. malloc'd XML in *out. 0 ok. */
int pain008_build(const pain008_hdr_t *h, const pain008_tx_t *tx, size_t n, char **out, size_t *len);
```
Content: `GrpHdr` (`MsgId`, `CreDtTm`, `NbOfTxs`, `CtrlSum` as `12.34`, `InitgPty/Nm`), `PmtInf` (`PmtInfId` = msg_id, `PmtMtd` DD, `BtchBookg` true, `NbOfTxs`, `CtrlSum`, `PmtTpInf/SvcLvl/Cd` SEPA, `LclInstrm/Cd` CORE, `SeqTp` RCUR, `ReqdColltnDt`, `Cdtr`, `CdtrAcct`, `CdtrAgt` (BIC or `Othr/Id NOTPROVIDED`), `ChrgBr` SLEV, `CdtrSchmeId` with `Prtry` SEPA), per tx `PmtId/EndToEndId`, `InstdAmt Ccy="EUR"`, `MndtRltdInf` (`MndtId`, `DtOfSgntr`), `DbtrAgt`, `Dbtr/Nm`, `DbtrAcct`, `RmtInf/Ustrd`. Amounts formatted from cents by integer math.

- [ ] **Step 1: Tests:** `test_translit_umlauts_and_ampersand` (`"Jürgen Müller-Weiß & Söhne"` → `"Juergen Mueller-Weiss + Soehne"`), `test_translit_truncates_70`, `test_translit_xml_specials` (`<>"` → `.`), `test_build_two_tx` (contains `<NbOfTxs>2</NbOfTxs>`, `<CtrlSum>57.05</CtrlSum>` for 1234 + 4471 cents, `<SeqTp>RCUR</SeqTp>`, `<ReqdColltnDt>2026-11-09</ReqdColltnDt>`, `<MndtId>OCS-000012</MndtId>`, `NOTPROVIDED` for empty BIC; writes the XML to `$TMPDIR/ocs-pain008-test.xml` for the XSD test). XSD: download `pain.008.001.02.xsd` (ISO 20022 message archive); if that is not possible, leave the test skipped and note it in the backlog.
- [ ] **Step 2–4.** **Step 5:** commit `feat(core): pain.008.001.02 writer with SEPA transliteration`.

---

### Task 6: PDF writer and invoice document

**Files:** Create `core/pdf.{h,c}`, `core/invoice_doc.{h,c}`, `test/test_pdf.c`; Modify `CMakeLists.txt`.

**Interfaces (produces):**
```c
/* pdf.h — A4 portrait (595 x 842 pt), uncompressed content streams, fonts Helvetica (0) and
 * Helvetica-Bold (1), both /WinAnsiEncoding. Errors are sticky; pdf_finish reports them. */
typedef struct pdf pdf_t;
pdf_t *pdf_new(void);
void pdf_page(pdf_t *p);                                               /* starts a new page */
void pdf_text(pdf_t *p, int font, int size, int x, int y, const char *utf8);
void pdf_text_right(pdf_t *p, int font, int size, int x_right, int y, const char *utf8);
void pdf_hline(pdf_t *p, int x1, int x2, int y);
int  pdf_text_width(int font, int size, const char *utf8);            /* 1/1000 pt units / 1000, rounded up */
int  pdf_finish(pdf_t *p, char **out, size_t *len);                   /* malloc'd file; 0 ok; frees p */
/* UTF-8 to WinAnsi (cp1252): Latin-1 range, € 0x80, „ 0x84, “ 0x93, – 0x96; others '?'. */
int  pdf_winansi(const char *utf8, char *out, size_t cap);

/* invoice_doc.h */
typedef struct {
  const settings_t *s; const invoice_t *inv; const invoice_line_t *lines; size_t n_lines;
  const char *mandate_ref, *iban_masked;   /* NULL for total 0 */
  const char *collection_date;             /* "YYYY-MM-DD"; NULL for total 0 */
} invoice_doc_t;
int invoice_pdf(const invoice_doc_t *d, char **out, size_t *len);
/* Mail body (pre-notification), length or -1. */
int invoice_mail_body(const invoice_doc_t *d, const char *user_first_line_name, char *out, size_t cap);
void invoice_subject(const invoice_t *inv, char *out, size_t cap);   /* "Rechnung 17 – Oktober 2026" */
void money_de(int64_t cents, char *out, size_t cap);                  /* "1.234,56 €", "-0,05 €" */
```
Helvetica / Helvetica-Bold widths: the Adobe Core 14 AFM values for codes 32..255 in WinAnsi order, as two `static const unsigned short[224]` tables.

PDF layout (pt, origin bottom left): sender line `assoc_name · street · zip city` 8 pt at y 760; addressee block 11 pt from y 720 (name, street, zip city); right column at x 400: „Rechnung“ bold 16, „Nr. 17“, „Datum: 10.11.2026“ (local `issued_at`), „Zeitraum: Oktober 2026“; table from y 600, one row per line, 9 pt, 14 pt row pitch, columns: Datum (local `dd.mm.`), Zeit (`hh:mm–hh:mm`), Auto, Tag h, Nacht h, Ungenutzt h (hours with one decimal via integer minutes: `min/60`,`(min%60)*10/60`), km, Gebühren (late + cancel fee), Betrag (right aligned at x 555); line text below the date when special: „Vereinsfahrt“, „Storniert“, „Nicht angetreten“, „Deckelung −x,xx €“ if `cap_cut_cents > 0`. New page when y < 120 (header repeated small: „Rechnung 17 – Seite 2“). After the table: rule, „Gesamtbetrag“ bold + amount. Then the pre-notification block and the footer (`invoice_footer`, split at `\n`, 8 pt, from y 60 up).

Pre-notification text (total > 0, also first paragraph of the mail):
`Wir ziehen den Gesamtbetrag von %amount am %dd.mm.yyyy per SEPA-Lastschrift von deinem Konto %iban_masked ein. Mandatsreferenz: %ref, Gläubiger-ID: %creditor_id.`
Total 0: `Es wird kein Betrag eingezogen.`
Mail body: `Hallo %name,\n\nanbei deine Rechnung %number für %period_label.\n\n%prenotification\n\nViele Grüße\n%assoc_name\n`.

- [ ] **Step 1: Tests (`test_pdf.c`):** `test_winansi_umlauts_euro` (`"Grüße €"` → `47 72 FC DF 65 20 80`), `test_pdf_xref_offsets` (parse `startxref` and the xref table; each offset points at `"<n> 0 obj"`), `test_pdf_header_and_encoding` (starts `%PDF-1.4`, contains `/WinAnsiEncoding`, ends `%%EOF\n`), `test_invoice_pdf_paginates` (60 lines → `/Count 3`), `test_invoice_pdf_contains_amount` (content stream contains `(57,05 \200)` i.e. WinAnsi-escaped amount), `test_mail_body_prenotification` (exact string for 5705 cents, `"2026-11-09"` → `09.11.2026`, `OCS-000012`, `DE89 **** 3000`), `test_mail_body_zero`, `test_money_de` (`123456` → `1.234,56 €`, `5` → `0,05 €`), `test_subject`.
- [ ] **Step 2–4.** Check by eye once: `pdftotext` on a generated sample (manual, if installed).
- [ ] **Step 5:** commit `feat(core): PDF writer and invoice document`.

---

### Task 7: Billing API — draft, finalize, invoiced lock, settings

**Files:** Create `http/api_billing.c`, `test/test_api_billing.c`; Modify `http/api.c` (routes), `http/api_internal.h`, `http/api_admin.c`, `test/helpers.{h,c}` if helpful (e.g. `th_add_done_booking`), `CMakeLists.txt`.

**Routes (all MIN_ADMIN):**
```
GET    /api/admin/billing                      list runs (newest first): run JSON + n_invoices, total_cents
POST   /api/admin/billing?period=YYYY-MM       201 run JSON (draft)
GET    /api/admin/billing/{id}                 run detail (below)
DELETE /api/admin/billing/{id}                 draft only
POST   /api/admin/billing/{id}/finalize
POST   /api/admin/billing/{id}/send            (Task 8)
GET    /api/admin/billing/{id}/sepa.xml        (Task 8)
GET    /api/admin/invoices/{number}/pdf        (Task 8)
```
Run detail: `{"run":{...}, "users":[{"user_id","name","email","mandate":{"ref","iban_masked"}|null, "total_cents","lines":[invoice_line JSON], "invoice":{"number","mailed_at"}|null}], "total_cents", "blockers":{"km_pending":[booking ids],"no_tariff":[booking ids],"no_mandate":[user ids]}}`. Draft: computed live (after `api_expire_bookings(a, now)`) from `billing_includes` + `price_booking` with the tariff at `booked_start` (`db_tariff_at`), users sorted by name, lines by start. Final: from stored invoices/lines; `blockers` empty.

Errors (German messages):
- create: bad/missing period 400 `invalid`; `period_end > now` 400 `period_open` „Der Monat ist noch nicht abgeschlossen.“; a draft exists 409 `draft_exists` „Es gibt bereits einen offenen Abrechnungsentwurf.“; a run with period ≥ requested 409 `period_exists` „Für diesen oder einen späteren Monat gibt es schon einen Abrechnungslauf.“.
- delete/finalize on final: 409 `not_draft` „Der Abrechnungslauf ist bereits abgeschlossen.“.
- finalize: assoc name/address, `creditor_id` or `creditor_iban` empty → 409 `no_creditor` „Bitte zuerst Vereins- und Gläubigerdaten eintragen.“; any blocker → 409 `blocked` „Abschluss nicht möglich.“ with body field `blockers` as in the detail (extend `api_error` use with a custom JSON body).
- finalize success: numbers from `db_next_invoice_number`, ascending by user name, one invoice per user with ≥ 1 line (including total 0), addressee snapshot, `created_at = now`; run `state final`, `finalized_at now`; one `db_finalize_run`. 200 with the run detail.
- admin booking `PUT`/`DELETE` with `invoice_id != 0`: 409 `invoiced` „Die Buchung ist bereits abgerechnet.“ (before any other check). `booking_to_json` includes `invoice_id`.
- settings GET/PUT gain `sepa_lead_days` (required on PUT, 5..30 else 400 `invalid`) and `invoice_footer` (required, < 512 bytes, stored trimmed).

- [ ] **Step 1: Tests (`test_api_billing.c`):** `test_create_requires_closed_month`, `test_create_draft_exists_409`, `test_create_not_ascending_409`, `test_draft_lists_users_lines_totals` (two users, one with two trips; hand-computed totals with a simple tariff), `test_draft_zero_invoice_club_only` (user with only a club trip: one line total 0, no blocker even without mandate), `test_draft_no_line_no_user` (cancelled fee 0 and other month after period: user absent), `test_draft_straggler_included` (booking of an earlier month without invoice), `test_finalize_blocked_km_pending`, `test_finalize_blocked_no_tariff`, `test_finalize_blocked_no_mandate` (user via `th_add_user_bare` with total > 0; body `blockers.no_mandate == [id]`), `test_finalize_gapless_numbers` (two runs → 1,2 then 3), `test_finalize_locks_bookings` (admin PUT → 409 `invoiced`), `test_finalize_uses_current_state` (GET draft, admin changes km, finalize: line km is the new value), `test_finalize_twice_409`, `test_delete_draft`, `test_settings_lead_days_range` (4 and 31 → 400; missing → 400).
- [ ] **Step 2–4.** **Step 5:** commit `feat(api): billing run draft and finalize`.

---

### Task 8: Send, SEPA XML, PDFs

**Files:** Modify `http/api_billing.c`, `test/test_api_billing.c`.

**Behavior of `POST /api/admin/billing/{id}/send`:**
1. Draft → 409 `not_final` „Der Abrechnungslauf ist noch nicht abgeschlossen.“.
2. First send (`sent_at == 0`): every invoice with total > 0 needs `db_mandate_current` of its user, else 409 `blocked` with `blockers.no_mandate`, nothing changed. Then `collection_date = billing_collection_date(now, sepa_lead_days)`; per invoice: `issued_at = now`, `mandate_id` (0 for total 0), write PDF (`store_write_atomic` to `db_invoice_pdf_path`); if any invoice > 0, build XML (`msg_id` `"OCS-RUN-<id>"`, `created` from now UTC, creditor from settings, per tx `end_to_end` `"OCS-RE-<number>"`, name = mandate holder, `mandate_date` = local date of mandate `created_at`, remittance `"Rechnung <number> Carsharing <period label>"`) to `data/billing/run-<id>-pain008.xml`; then `db_put_invoices` and `db_put_run` (`sent_at`, `collection_date`, `sepa_file`). Storage failure → 500, retry redoes this step.
3. Every invoice with `mailed_at == 0`: mail with PDF attachment (`Rechnung-<number>.pdf`, `application/pdf`, read back from disk), subject `invoice_subject`, body `invoice_mail_body`; on success set `mailed_at = now` (one `db_put_invoices` at the end).
4. 200 `{"run":{...},"sent":n,"failed":[invoice numbers]}`.

`GET .../sepa.xml`: `sepa_file` empty → 404 `not_found`; else the file, `application/xml`. `GET /api/admin/invoices/{n}/pdf`: unknown or not yet issued → 404; else the file, `application/pdf`. (`api_res_t.content_type` is 32 bytes: both fit.)

- [ ] **Step 1: Tests:** `test_send_draft_409`, `test_send_mails_with_pdf` (mail count = invoices, `th_last_mail_att_name() == "Rechnung-1.pdf"`, body contains collection date and `OCS-`), `test_send_zero_invoice_no_prenotification` (body contains „Es wird kein Betrag eingezogen.“; not in XML: `NbOfTxs` counts only > 0), `test_send_sets_collection_date` (fixed `now` → expected date from Task 2 rules), `test_send_retry_mails_only_unmailed` (`th_mail_fail(1)` first: `failed` lists all, `collection_date` set; second send with mail ok: count = invoices, collection date and XML bytes unchanged; third send: 0 mails), `test_send_uses_mandate_current_at_send` (user replaces the mandate between finalize and send: XML has the new mandate ref, IBAN of the new mandate), `test_send_blocked_mandate_revoked_after_finalize`, `test_sepa_xml_download` (content type, starts `<?xml`, 404 before send), `test_invoice_pdf_download` (404 before send, `%PDF` after), `test_api_responses_never_full_iban` (no response of the flow contains `DE89370400440532013000`).
- [ ] **Step 2–4.** **Step 5:** commit `feat(api): send invoices, SEPA XML, invoice PDFs`.

---

### Task 9: Web — billing admin view

**Files:** Create `web/js/billing.js`, `web/js/views/admin_billing.js`, `web/test/billing.test.mjs`; Modify admin navigation (`web/js/admin_ui.js` / `web/js/app.js` route `#/admin/abrechnung`), `web/js/views/admin_settings.js` (fields „Vorlauf Lastschrift (Tage)“, „Rechnungsfußzeile“ textarea), `web/js/views/admin_bookings.js` (booking with `invoice_id`: badge „Rechnung N“, edit/cancel hidden), `web/sw.js`.

**Interfaces (`billing.js`, pure):**
```js
export function lastMonth(now /* Date */) // "YYYY-MM" of the previous Berlin month
export function periodLabel(period)       // "2026-10" -> "Oktober 2026"
export function dateDe(iso)               // "2026-11-09" -> "09.11.2026"
export function blockerLines(blockers, usersById) // German lines: "Buchung 12: km fehlen", "Buchung 7: kein Tarif", "Anna B.: kein SEPA-Mandat"
```
View: period select (default `lastMonth`) + „Entwurf erstellen“; list of runs (period label, state „Entwurf“/„Abgeschlossen“/„Versendet“, total); detail: blockers box (links to `#/admin/buchungen`), table per user (name, mandate ref or „kein Mandat“, total, expandable lines), buttons by state: draft „Neu berechnen“ (reload), „Löschen“, „Abschließen“ (confirm dialog naming the invoice count and total); final „Versenden“ (confirm „Rechnungen jetzt per Mail versenden? Der Einzug erfolgt frühestens in N Tagen.“ with N = `sepa_lead_days`); after send: collection date, „SEPA-XML herunterladen“ link, per invoice „PDF“ link and mailed state, „Erneut versenden“ when some `mailed_at == 0`.

- [ ] **Step 1: Tests (`billing.test.mjs`):** `lastMonth(new Date('2026-11-01T00:30:00+01:00')) === '2026-10'`, `lastMonth(new Date('2026-01-15T12:00:00Z')) === '2025-12'`, `periodLabel('2026-03') === 'März 2026'`, `dateDe`, `blockerLines` for all three kinds.
- [ ] **Step 2–4:** `node --test web/test/` passes; check the view in the running app (`run` skill or `ocs-server` locally) through one full run.
- [ ] **Step 5:** add new files to `SHELL`, bump `CACHE`; commit `feat(web): billing run admin view`.

---

### Task 10: Docs

**Files:** `docs/spec.md`, `docs/backlog.md`, `deploy/vps/README.md`.

- [ ] **Step 1:** spec: §4 booking `invoice_id`; invoice (fields of `invoice_t`, lines table, PDF path), billing_run (fields, XML path), settings `sepa_lead_days` (5..30, default 5), `invoice_footer`; §9 steps reworded (finalize = numbers + lock; send = collection date, PDFs, XML, mails; retry), inclusion rule (builder call 5), ascending runs, `invoiced` lock, mandate text v2 (5-day pre-notification, creditor address), `bic_required`; §13 new routes; §15 Phase 6 marked done with plan path and merge date.
- [ ] **Step 2:** backlog: remove the three `[P6]` SEPA items now done (transliteration, BIC for non-EEA IBANs, creditor address in the mandate text); add new findings; if the XSD could not be vendored, add `[any] pain008 XSD check skipped`. Add `[P7]` "billing run done" mail reminder if not covered by spec.
- [ ] **Step 3:** deploy README: Hibiscus import steps (SEPA-Sammellastschrift importieren, file from „SEPA-XML herunterladen“), the PO acceptance check "XML imports into Hibiscus" as a manual step.
- [ ] **Step 4:** commit `docs: phase 6 billing run in spec`.

---

## Acceptance

- `ctest` and `node --test web/test/` green; `pain008_xsd` passes (or skipped with backlog note).
- Manual: one month billed end-to-end in a local instance (draft → finalize → send to a test mailbox → PDF opens and shows umlauts/€ → XML downloaded).
- Product owner: XML imported into Hibiscus without error (done-criterion of Phase 6).
