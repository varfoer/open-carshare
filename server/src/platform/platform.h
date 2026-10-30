#ifndef OCS_PLATFORM_PLATFORM_H
#define OCS_PLATFORM_PLATFORM_H

/* Durability helpers. 0 ok, -1 fail. */
int platform_fsync_file(const char *path);
int platform_fsync_dir(const char *dir);

#endif
