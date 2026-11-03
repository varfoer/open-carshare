#ifndef OCS_CORE_PAIN008_H
#define OCS_CORE_PAIN008_H

#include <stddef.h>
#include <stdint.h>

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

/* One PmtInf (CORE, RCUR). Names/remittance pass through sepa_translit. malloc'd XML in *out. 0 ok.
 * -1 on bad input (n == 0, amount_cents <= 0) or allocation failure. */
int pain008_build(const pain008_hdr_t *h, const pain008_tx_t *tx, size_t n, char **out, size_t *len);

#endif
