#include "store/store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

static int blank(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') return 0;
  return 1;
}

static int handle_line(const char *s, size_t n, int (*cb)(const cJSON *, void *), void *ctx) {
  if (blank(s, n)) return 0;
  /* s is NUL-terminated at s[n] by the caller */
  cJSON *j = cJSON_ParseWithLengthOpts(s, n + 1, NULL, 1);
  if (!j) return -1;
  int rc = cb(j, ctx);
  cJSON_Delete(j);
  return rc == 0 ? 0 : -1;
}

int store_read_lines(const char *path, int (*cb)(const cJSON *, void *), void *ctx) {
  FILE *f = fopen(path, "rb");
  if (!f) return errno == ENOENT ? 0 : -1;
  size_t cap = 256, n = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fclose(f);
    return -1;
  }
  int rc = 0, c;
  while (rc == 0 && (c = fgetc(f)) != EOF) {
    if (c == '\n') {
      buf[n] = '\0';
      rc = handle_line(buf, n, cb, ctx);
      n = 0;
      continue;
    }
    if (n + 1 >= cap) {
      char *nb = realloc(buf, cap * 2);
      if (!nb) {
        rc = -1;
        break;
      }
      buf = nb;
      cap *= 2;
    }
    buf[n++] = (char)c;
  }
  if (rc == 0 && ferror(f)) rc = -1;
  if (rc == 0 && n > 0) buf[n] = '\0';
  if (rc == 0 && n > 0) rc = handle_line(buf, n, cb, ctx);
  free(buf);
  fclose(f);
  return rc;
}

int store_write_atomic(const char *path, const char *data, size_t len) {
  char tmp[512], dir[512];
  int k = snprintf(tmp, sizeof tmp, "%s.tmp", path);
  if (k < 0 || (size_t)k >= sizeof tmp) return -1;
  k = snprintf(dir, sizeof dir, "%s", path);
  if (k < 0 || (size_t)k >= sizeof dir) return -1;
  char *slash = strrchr(dir, '/');
  if (!slash) strcpy(dir, ".");
  else if (slash == dir) dir[1] = '\0';
  else *slash = '\0';

  FILE *f = fopen(tmp, "wb");
  if (!f) return -1;
  int ok = (len == 0 || fwrite(data, 1, len, f) == len) && fflush(f) == 0;
  if (fclose(f) != 0) ok = 0;
  if (ok && platform_fsync_file(tmp) != 0) ok = 0;
  if (ok && rename(tmp, path) != 0) ok = 0;
  if (!ok) {
    remove(tmp);
    return -1;
  }
  return platform_fsync_dir(dir);
}
