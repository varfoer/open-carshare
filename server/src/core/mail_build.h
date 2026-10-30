#ifndef OCS_CORE_MAIL_BUILD_H
#define OCS_CORE_MAIL_BUILD_H

#include <stddef.h>
#include <stdint.h>

/* Build an RFC 5322 text/plain UTF-8 message into buf (NUL-terminated).
 * Non-ASCII subject becomes RFC 2047 B-encoded words. Body LF -> CRLF.
 * Returns length (excluding NUL), or -1 if n too small or from/to/subject
 * contain CR/LF. date = epoch seconds, rendered in UTC. */
int mail_build(char *buf, size_t n, const char *from, const char *to,
               const char *subject, const char *body, int64_t date);

#endif
