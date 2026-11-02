#include "store/store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
  if (!j) return -2;
  int rc = cb(j, ctx);
  cJSON_Delete(j);
  return rc == 0 ? 0 : -1;
}

/* log != 0: any unterminated last line is treated as torn and skipped without
 * parsing; *keep (if non-NULL) is set to the offset where it starts, else -1. */
static int read_impl(const char *path, int (*cb)(const cJSON *, void *), void *ctx, int log,
                     long *keep) {
  if (keep) *keep = -1;
  FILE *f = fopen(path, "rb");
  if (!f) return errno == ENOENT ? 0 : -1;
  size_t cap = 256, n = 0;
  char *buf = malloc(cap);
  if (!buf) {
    fclose(f);
    return -1;
  }
  int rc = 0, c;
  long off = 0, line_start = 0;
  while (rc == 0 && (c = fgetc(f)) != EOF) {
    off++;
    if (c == '\n') {
      buf[n] = '\0';
      rc = handle_line(buf, n, cb, ctx);
      if (rc == -2) rc = -1;
      n = 0;
      line_start = off;
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
  if (rc == 0 && n > 0) {
    if (log) {
      if (keep) *keep = line_start;
    } else {
      buf[n] = '\0';
      rc = handle_line(buf, n, cb, ctx);
      if (rc == -2) rc = -1;
    }
  }
  free(buf);
  fclose(f);
  return rc;
}

int store_read_lines(const char *path, int (*cb)(const cJSON *, void *), void *ctx) {
  return read_impl(path, cb, ctx, 0, NULL);
}

int store_read_log(const char *path, int (*cb)(const cJSON *, void *), void *ctx, long *keep) {
  return read_impl(path, cb, ctx, 1, keep);
}

int store_append(const char *path, const char *data, size_t len) {
  int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) != 0) {
    close(fd);
    return -1;
  }
  off_t before = st.st_size;
  int ok = 1;
  size_t done = 0;
  while (ok && done < len) {
    ssize_t w = write(fd, data + done, len - done);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) ok = 0;
    else done += (size_t)w;
  }
  if (ok && fsync(fd) != 0) ok = 0;
  if (!ok && ftruncate(fd, before) != 0) {
    /* nothing more to do: caller still sees failure */
  }
  if (close(fd) != 0) ok = 0;
  return ok ? 0 : -1;
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
  /* The rename already made the new content visible. A dir fsync failure only
   * degrades power-loss durability, so it is not reported as a write failure. */
  (void)platform_fsync_dir(dir);
  return 0;
}

int store_read_file(const char *path, size_t max, char **out, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  char *buf = malloc(max + 2);
  if (!buf) {
    fclose(f);
    return -1;
  }
  size_t n = fread(buf, 1, max + 1, f);
  int bad = ferror(f) || n > max;
  fclose(f);
  if (bad) {
    free(buf);
    return -1;
  }
  buf[n] = '\0';
  *out = buf;
  *len = n;
  return 0;
}

int store_ensure_dir(const char *path) {
  return (mkdir(path, 0700) == 0 || errno == EEXIST) ? 0 : -1;
}
