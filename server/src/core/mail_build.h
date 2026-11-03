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

typedef struct { const char *filename, *content_type; const void *data; size_t len; } mail_att_t;

/* att NULL: identical output to mail_build (text/plain). Otherwise multipart/mixed with boundary
 * "=_ocs_part_0": text part as in mail_build, attachment base64 (76-char lines, CRLF),
 * Content-Disposition: attachment; filename="<filename>" (ASCII only, no '"' or '\\'; -1 otherwise). */
int mail_build_att(char *buf, size_t n, const char *from, const char *to, const char *subject,
                   const char *body, int64_t date, const mail_att_t *att);

/* Buffer size enough for mail_build_att (from/to/subject together under ~2 KiB). */
size_t mail_size_bound(size_t body_len, size_t att_len);

/* SMTP envelope sender (MAIL FROM) for a configured mail_from value, which may
 * be "Name <addr>" or a bare "addr". Writes the part between '<' and '>' if a
 * '<' is present, else the whole value with surrounding blanks trimmed.
 * Returns 0, or -1 if from contains CR/LF, the '>' is missing, the address is
 * empty or contains blanks, or it does not fit out[n] with NUL. The From:
 * header keeps the full value (mail_build). */
int mail_envelope_addr(const char *from, char *out, size_t n);

#endif
