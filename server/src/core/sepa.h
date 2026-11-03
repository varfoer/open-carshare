#ifndef OCS_CORE_SEPA_H
#define OCS_CORE_SEPA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/model.h"

#define MANDATE_TEXT_VERSION 2
#define SEPA_PRENOTIFY_MIN_DAYS 5
#define SEPA_HOLDER_MAX_CHARS 70

/* Remove spaces, uppercase a-z. 0 ok; -1 on any other char outside [A-Za-z0-9 ] or if it does not fit. */
int  sepa_normalize(const char *in, char *out, size_t cap);
bool iban_valid(const char *iban);         /* normalized input */
bool bic_valid(const char *bic);           /* normalized: 4 letters, 2 letters, 2 alnum, optional 3 alnum */
bool creditor_id_valid(const char *ci);    /* normalized */
void iban_mask(const char *iban, char *out, size_t cap);   /* "DE89 **** 3000"; "" if strlen < 8 */
void mandate_ref(int64_t id, char *out, size_t cap);       /* "OCS-000012" */
bool iban_needs_bic(const char *iban);     /* normalized; true for AD CH GB GI MC SM VA (non-EEA) */
int  mandate_text(char *out, size_t cap, const settings_t *s); /* uses assoc_name, address, creditor_id; length or -1 */
/* Confirmation mail text for a given mandate; address from s, name and creditor ID from the mandate snapshot; length or -1. */
int  mandate_mail_body(char *out, size_t cap, const char *user_name, const mandate_t *m, const settings_t *s);

#endif
