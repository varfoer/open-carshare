#include "core/mail_build.h"

#include <string.h>

typedef struct {
  char *buf;
  size_t n, len;
  int over;
} out_t;

static void put(out_t *o, const char *s, size_t k) {
  /* keep one byte for the NUL */
  if (o->over || k >= o->n - o->len) {
    o->over = 1;
    return;
  }
  memcpy(o->buf + o->len, s, k);
  o->len += k;
}

static void puts_(out_t *o, const char *s) { put(o, s, strlen(s)); }

static void b64_padded(out_t *o, const unsigned char *in, size_t len) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0;
  while (i < len) {
    size_t r = len - i;
    unsigned v = (unsigned)in[i] << 16;
    if (r > 1) v |= (unsigned)in[i + 1] << 8;
    if (r > 2) v |= in[i + 2];
    char q[4] = {A[(v >> 18) & 63], A[(v >> 12) & 63],
                 r > 1 ? A[(v >> 6) & 63] : '=', r > 2 ? A[v & 63] : '='};
    put(o, q, 4);
    i += 3;
  }
}

static int has_crlf(const char *s) { return strpbrk(s, "\r\n") != NULL; }

static int is_ascii(const char *s) {
  for (; *s; s++)
    if ((unsigned char)*s >= 0x80) return 0;
  return 1;
}

/* Days since 1970-01-01 -> civil date (Howard Hinnant). */
static void civil_from_days(int64_t z, int *y, int *m, int *d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yy + (*m <= 2));
}

static void put_num(out_t *o, int v, int width) {
  char t[12];
  int i = 0;
  do {
    t[i++] = (char)('0' + v % 10);
    v /= 10;
  } while (v > 0);
  while (i < width) t[i++] = '0';
  while (i > 0) put(o, &t[--i], 1);
}

static void put_date(out_t *o, int64_t date) {
  static const char *const WD[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *const MO[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int64_t days = date >= 0 ? date / 86400 : -((-date + 86399) / 86400);
  int64_t sod = date - days * 86400;
  int64_t wd = (days + 4) % 7; /* 1970-01-01 was Thursday */
  if (wd < 0) wd += 7;
  int y, m, d;
  civil_from_days(days, &y, &m, &d);
  puts_(o, "Date: ");
  puts_(o, WD[wd]);
  puts_(o, ", ");
  put_num(o, d, 2);
  puts_(o, " ");
  puts_(o, MO[m - 1]);
  puts_(o, " ");
  put_num(o, y, 4);
  puts_(o, " ");
  put_num(o, (int)(sod / 3600), 2);
  puts_(o, ":");
  put_num(o, (int)(sod % 3600 / 60), 2);
  puts_(o, ":");
  put_num(o, (int)(sod % 60), 2);
  puts_(o, " +0000\r\n");
}

/* Max raw bytes per encoded word: 45 -> 60 b64 chars + 12 = 72 <= 75. */
#define WORD_RAW 45

static void put_subject(out_t *o, const char *s) {
  puts_(o, "Subject: ");
  if (is_ascii(s)) {
    puts_(o, s);
  } else {
    size_t len = strlen(s), i = 0;
    int first = 1;
    while (i < len) {
      size_t k = len - i < WORD_RAW ? len - i : WORD_RAW;
      if (i + k < len)
        while (k > 0 && ((unsigned char)s[i + k] & 0xC0) == 0x80) k--;
      if (k == 0) k = len - i < WORD_RAW ? len - i : WORD_RAW; /* malformed UTF-8 */
      if (!first) puts_(o, "\r\n ");
      first = 0;
      puts_(o, "=?UTF-8?B?");
      b64_padded(o, (const unsigned char *)s + i, k);
      puts_(o, "?=");
      i += k;
    }
  }
  puts_(o, "\r\n");
}

int mail_build(char *buf, size_t n, const char *from, const char *to,
               const char *subject, const char *body, int64_t date) {
  if (!buf || n == 0 || !from || !to || !subject || !body) return -1;
  if (has_crlf(from) || has_crlf(to) || has_crlf(subject)) return -1;
  out_t o = {buf, n, 0, 0};
  put_date(&o, date);
  puts_(&o, "From: ");
  puts_(&o, from);
  puts_(&o, "\r\nTo: ");
  puts_(&o, to);
  puts_(&o, "\r\n");
  put_subject(&o, subject);
  puts_(&o, "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=UTF-8\r\n"
            "Content-Transfer-Encoding: 8bit\r\n\r\n");
  size_t blen = strlen(body);
  for (size_t i = 0; i < blen; i++) {
    if (body[i] == '\r' && i + 1 < blen && body[i + 1] == '\n') continue;
    if (body[i] == '\n') puts_(&o, "\r\n");
    else put(&o, &body[i], 1);
  }
  if (blen > 0 && body[blen - 1] != '\n') puts_(&o, "\r\n");
  if (o.over) return -1;
  buf[o.len] = '\0';
  return (int)o.len;
}
