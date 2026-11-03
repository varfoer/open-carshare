#ifndef OCS_CORE_PDF_H
#define OCS_CORE_PDF_H

#include <stddef.h>

/* Minimal PDF writer: A4 portrait (595 x 842 pt, origin bottom left), uncompressed content
 * streams, fonts Helvetica (0) and Helvetica-Bold (1), both /WinAnsiEncoding.
 * A new pdf has no page: call pdf_page() before the first drawing call. Errors (allocation
 * failure, drawing before the first page, bad font, text longer than 511 WinAnsi bytes,
 * no page at all) are sticky and reported by pdf_finish(); nothing is truncated silently. */
typedef struct pdf pdf_t;
pdf_t *pdf_new(void);
void pdf_page(pdf_t *p);                                               /* starts a new page */
void pdf_text(pdf_t *p, int font, int size, int x, int y, const char *utf8);
void pdf_text_right(pdf_t *p, int font, int size, int x_right, int y, const char *utf8);
void pdf_hline(pdf_t *p, int x1, int x2, int y);
/* Width in whole points, rounded up (AFM widths in 1/1000 em). Unmapped WinAnsi codes count 0. */
int  pdf_text_width(int font, int size, const char *utf8);
int  pdf_finish(pdf_t *p, char **out, size_t *len);                   /* malloc'd file; 0 ok, -1 error; frees p */
/* UTF-8 to WinAnsi (cp1252): Latin-1 range, € 0x80, „ 0x84, “ 0x93, – 0x96; others '?'.
 * Writes a NUL-terminated string; returns its length, or -1 if it does not fit in cap. */
int  pdf_winansi(const char *utf8, char *out, size_t cap);

#endif
