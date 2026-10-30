#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_LEN 1024

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  return s;
}

/* Copy v into dst[cap] only if it fits; never truncates. */
static int set(char *dst, size_t cap, const char *v) {
  size_t n = strlen(v);
  if (n >= cap) return -1;
  memcpy(dst, v, n + 1);
  return 0;
}

int config_load(const char *path, config_t *cfg) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  memset(cfg, 0, sizeof *cfg);
  int rc = 0;
  char line[LINE_MAX_LEN];
  while (rc == 0 && fgets(line, sizeof line, f)) {
    if (!strchr(line, '\n') && !feof(f)) { rc = -1; break; } /* line too long */
    char *s = trim(line);
    if (*s == 0 || *s == '#') continue;
    char *eq = strchr(s, '=');
    if (!eq) { rc = -1; break; }
    *eq = 0;
    char *k = trim(s), *v = trim(eq + 1);
    if (strcmp(k, "listen") == 0) rc = set(cfg->listen, sizeof cfg->listen, v);
    else if (strcmp(k, "data_dir") == 0) rc = set(cfg->data_dir, sizeof cfg->data_dir, v);
    else if (strcmp(k, "web_dir") == 0) rc = set(cfg->web_dir, sizeof cfg->web_dir, v);
    else if (strcmp(k, "base_url") == 0) rc = set(cfg->base_url, sizeof cfg->base_url, v);
    else if (strcmp(k, "smtp_url") == 0) rc = set(cfg->smtp_url, sizeof cfg->smtp_url, v);
    else if (strcmp(k, "smtp_user") == 0) rc = set(cfg->smtp_user, sizeof cfg->smtp_user, v);
    else if (strcmp(k, "smtp_pass") == 0) rc = set(cfg->smtp_pass, sizeof cfg->smtp_pass, v);
    else if (strcmp(k, "mail_from") == 0) rc = set(cfg->mail_from, sizeof cfg->mail_from, v);
    else rc = -1;
  }
  if (ferror(f)) rc = -1;
  fclose(f);
  if (rc != 0) return -1;
  if (!cfg->data_dir[0] || !cfg->web_dir[0] || !cfg->base_url[0]) return -1;
  if (!cfg->listen[0]) snprintf(cfg->listen, sizeof cfg->listen, "http://127.0.0.1:8080");
  return 0;
}
