#ifndef OCS_CONFIG_H
#define OCS_CONFIG_H

#include "app.h"

/* key = value, '#' comments, blank lines. Required: data_dir, web_dir, base_url.
 * listen defaults to http://127.0.0.1:8080. Optional backup_* keys (used by ocs-backup.sh):
 * backup_recipient "age1" + [a-z0-9], backup_mail_to a valid address (normalized),
 * backup_dir (default /var/backups/ocs), backup_max_mail_bytes 1..100000000 (default 20000000).
 * igloo_client_id/_secret (default empty = not configured), igloo_api_url (default https://api.igloohome.co)
 * and igloo_token_url (default https://auth.igloohome.co/oauth2/token): https only, no trailing "/".
 * Unknown key, over-long value,
 * malformed line, unreadable file or missing required key -> -1. */
int config_load(const char *path, config_t *cfg);

#endif
