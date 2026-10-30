#ifndef OCS_CORE_EMAIL_H
#define OCS_CORE_EMAIL_H

/* Trim and lowercase (ASCII) into out. 0 ok, -1 if not an address with
 * exactly one '@' (not first or last), contains whitespace, or is >= 128 bytes. */
int email_normalize(const char *in, char out[128]);

#endif
