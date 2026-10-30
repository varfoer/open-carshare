#ifndef OCS_STORE_STORE_H
#define OCS_STORE_STORE_H

#include <stddef.h>
#include "cJSON.h"

/* Calls cb for each non-blank line parsed as JSON. Missing file -> 0 (empty).
 * Unparsable line, read error or cb != 0 -> -1. obj is only valid during cb. */
int store_read_lines(const char *path, int (*cb)(const cJSON *obj, void *ctx), void *ctx);

/* Writes path.tmp, fsyncs, renames over path, fsyncs the dir.
 * -1 on any failure; path itself is left untouched. */
int store_write_atomic(const char *path, const char *data, size_t len);

#endif
