#include "platform/platform.h"

#include <fcntl.h>
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
