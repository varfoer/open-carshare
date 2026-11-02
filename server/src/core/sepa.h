#ifndef OCS_CORE_SEPA_H
#define OCS_CORE_SEPA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/model.h"

#define MANDATE_TEXT_VERSION 1
#define SEPA_HOLDER_MAX_CHARS 70

/* Remove spaces, uppercase a-z. 0 ok; -1 on any other char outside [A-Za-z0-9 ] or if it does not fit. */
int  sepa_normalize(const char *in, char *out, size_t cap);
bool iban_valid(const char *iban);         /* normalized input */
bool bic_valid(const char *bic);           /* normalized: 4 letters, 2 letters, 2 alnum, optional 3 alnum */
bool creditor_id_valid(const char *ci);    /* normalized */
void iban_mask(const char *iban, char *out, size_t cap);   /* "DE89 **** 3000"; "" if strlen < 8 */
void mandate_ref(int64_t id, char *out, size_t cap);       /* "OCS-000012" */
int  mandate_text(char *out, size_t cap, const char *creditor_name, const char *creditor_id); /* length or -1 */
/* Confirmation mail text for a given mandate; length or -1. */
int  mandate_mail_body(char *out, size_t cap, const char *user_name, const mandate_t *m);

#endif
