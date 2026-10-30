#include "core/email.h"

#include <ctype.h>
#include <string.h>

int email_normalize(const char *in, char out[128]) {
  while (*in && isspace((unsigned char)*in)) in++;
  size_t n = strlen(in);
  while (n > 0 && isspace((unsigned char)in[n - 1])) n--;
  if (n == 0 || n >= 128) return -1;
  size_t ats = 0;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '@') ats++;
    if (isspace(c)) return -1;
    out[i] = (char)tolower(c);
  }
  out[n] = 0;
  if (ats != 1 || out[0] == '@' || out[n - 1] == '@') return -1;
  return 0;
}
