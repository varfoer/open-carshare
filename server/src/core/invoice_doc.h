#ifndef OCS_CORE_INVOICE_DOC_H
#define OCS_CORE_INVOICE_DOC_H

#include <stddef.h>
#include <stdint.h>
#include "core/model.h"

typedef struct {
  const settings_t *s; const invoice_t *inv; const invoice_line_t *lines; size_t n_lines;
  const char *mandate_ref, *iban_masked;   /* NULL for total 0 */
  const char *collection_date;             /* "YYYY-MM-DD"; NULL for total 0 */
} invoice_doc_t;

/* A4 invoice with line table, total, pre-notification and footer. 0 ok (malloc'd *out), -1 on
 * error (allocation, missing mandate data for total > 0, text that does not fit). */
int invoice_pdf(const invoice_doc_t *d, char **out, size_t *len);
/* Mail body (pre-notification), length or -1 (does not fit, or mandate data missing for total > 0). */
int invoice_mail_body(const invoice_doc_t *d, const char *user_first_line_name, char *out, size_t cap);
void invoice_subject(const invoice_t *inv, char *out, size_t cap);   /* "Rechnung 17 – Oktober 2026" */
void money_de(int64_t cents, char *out, size_t cap);                  /* "1.234,56 €", "-0,05 €" */

#endif
