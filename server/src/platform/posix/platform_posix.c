#include "platform/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

static int fsync_path(const char *path, int flags) {
  int fd = open(path, flags);
  if (fd < 0) return -1;
  int rc = fsync(fd);
  if (close(fd) != 0) rc = -1;
  return rc == 0 ? 0 : -1;
}

int platform_fsync_file(const char *path) { return fsync_path(path, O_RDONLY); }
int platform_fsync_dir(const char *dir) { return fsync_path(dir, O_RDONLY | O_DIRECTORY); }

int64_t platform_now(void) { return (int64_t)time(NULL); }

int platform_random(void *buf, size_t n) {
  unsigned char *p = buf;
  while (n > 0) {
    ssize_t r = getrandom(p, n, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    p += r;
    n -= (size_t)r;
  }
  return 0;
}

int platform_mail_send(const char *smtp_url, const char *user, const char *pass,
                       const char *from, const char *to, const char *message) {
  (void)smtp_url; (void)user; (void)pass; (void)from; (void)to; (void)message;
  return -1;
}
