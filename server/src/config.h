#ifndef OCS_CONFIG_H
#define OCS_CONFIG_H

#include "app.h"

/* key = value, '#' comments, blank lines. Required: data_dir, web_dir, base_url.
 * listen defaults to http://127.0.0.1:8080. Unknown key, over-long value,
 * malformed line, unreadable file or missing required key -> -1. */
int config_load(const char *path, config_t *cfg);

#endif
