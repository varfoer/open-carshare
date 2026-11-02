#ifndef OCS_STORE_STORE_H
#define OCS_STORE_STORE_H

#include <stddef.h>
#include "cJSON.h"

/* Calls cb for each non-blank line parsed as JSON. Missing file -> 0 (empty).
 * Unparsable line, read error or cb != 0 -> -1. obj is only valid during cb. */
int store_read_lines(const char *path, int (*cb)(const cJSON *obj, void *ctx), void *ctx);

/* Like store_read_lines, but for append-only logs: a final line without a
 * trailing '\n' that does not parse is skipped (torn append), not an error. */
int store_read_log(const char *path, int (*cb)(const cJSON *obj, void *ctx), void *ctx);

/* As store_read_log; *keep is set to the byte offset where a skipped torn last
 * line starts (the end of the last complete line), or -1 if none was skipped. */
int store_read_log_keep(const char *path, int (*cb)(const cJSON *obj, void *ctx), void *ctx,
                        long *keep);

/* Appends data (O_APPEND, mode 0600, created if missing) and fsyncs. On any
 * failure truncates back to the size before the call and returns -1. */
int store_append(const char *path, const char *data, size_t len);

/* Writes path.tmp, fsyncs it, renames over path, then fsyncs the dir.
 * -1 only for failures before or at the rename: path is then untouched and the
 * tmp file is removed. Once rename succeeded the write is visible, so a dir
 * fsync failure still returns 0 (only power-loss durability is degraded). */
int store_write_atomic(const char *path, const char *data, size_t len);

/* Reads the whole file into a malloc'd, NUL-terminated buffer (caller frees *out).
 * 0 ok; -1 if missing, unreadable or larger than max bytes. */
int store_read_file(const char *path, size_t max, char **out, size_t *len);

/* mkdir mode 0700; an existing path (EEXIST) is success. 0 ok, -1 otherwise. */
int store_ensure_dir(const char *path);

#endif
