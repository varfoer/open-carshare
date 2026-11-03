#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "auth/password.h"
#include "config.h"
#include "core/email.h"
#include "core/mail_build.h"
#include "platform/platform.h"
#include "server.h"

#define PW_MIN_CHARS 10
#define PW_MAX_BYTES 1024

/* Logs every failure to stderr (recipient and reason only; never the SMTP
 * password or the mail body, which carries the token link). */
static int mail_cb(void *ctx, const char *to, const char *subject, const char *body,
                   const mail_att_t *att) {
  app_t *a = ctx;
  const char *why = NULL;
  char env_from[sizeof a->cfg.mail_from];
  char *buf = NULL;
  size_t cap = mail_size_bound(strlen(body), att ? att->len : 0);
  if (!a->cfg.smtp_url[0]) why = "smtp_url not configured";
  else if (mail_envelope_addr(a->cfg.mail_from, env_from, sizeof env_from) != 0)
    why = "invalid mail_from";
  else if (!(buf = malloc(cap))) why = "out of memory";
  else if (mail_build_att(buf, cap, a->cfg.mail_from, to, subject, body, platform_now(), att) < 0)
    why = "message could not be built";
  else if (platform_mail_send(a->cfg.smtp_url, a->cfg.smtp_user, a->cfg.smtp_pass, env_from, to,
                              buf) != 0)
    why = "SMTP send failed";
  free(buf);
  if (why) {
    fprintf(stderr, "ocs-server: mail to %s failed: %s\n", to, why);
    return -1;
  }
  return 0;
}

static size_t utf8_len(const char *s) {
  size_t n = 0;
  for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
  return n;
}

static int create_admin(app_t *a, const char *email_in, const char *name) {
  user_t u;
  memset(&u, 0, sizeof u);
  if (email_normalize(email_in, u.email) != 0) {
    fprintf(stderr, "ocs-server: invalid email address\n");
    return 1;
  }
  if (!*name || strlen(name) >= sizeof u.name) {
    fprintf(stderr, "ocs-server: name empty or too long (max %zu bytes)\n", sizeof u.name - 1);
    return 1;
  }
  char pw[PW_MAX_BYTES + 8];
  if (!fgets(pw, sizeof pw, stdin)) {
    fprintf(stderr, "ocs-server: no password on stdin\n");
    return 1;
  }
  size_t n = strlen(pw);
  if (n > 0 && pw[n - 1] == '\n') pw[--n] = 0;
  if (n > 0 && pw[n - 1] == '\r') pw[--n] = 0;
  if (n > PW_MAX_BYTES) {
    fprintf(stderr, "ocs-server: password too long (max %d bytes)\n", PW_MAX_BYTES);
    return 1;
  }
  if (utf8_len(pw) < PW_MIN_CHARS) {
    fprintf(stderr, "ocs-server: password must have at least %d characters\n", PW_MIN_CHARS);
    return 1;
  }
  if (db_user_by_email(&a->db, u.email)) {
    fprintf(stderr, "ocs-server: user %s already exists\n", u.email);
    return 1;
  }
  if (pw_hash(pw, u.pw_hash) != 0) {
    fprintf(stderr, "ocs-server: password hashing failed\n");
    return 1;
  }
  memset(pw, 0, sizeof pw);
  snprintf(u.name, sizeof u.name, "%s", name);
  u.role = ROLE_ADMIN;
  u.active = true;
  u.created_at = platform_now();
  if (db_put_user(&a->db, &u) != 0) {
    fprintf(stderr, "ocs-server: storage failure\n");
    return 1;
  }
  return 0;
}

static int usage(void) {
  fprintf(stderr, "usage: ocs-server -c <conf> [create-admin <email> <name>]\n");
  return 2;
}

int main(int argc, char **argv) {
  const char *conf = NULL;
  int i = 1;
  while (i < argc && argv[i][0] == '-') {
    if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) { conf = argv[i + 1]; i += 2; }
    else return usage();
  }
  if (!conf) return usage();
  int admin = 0;
  if (i < argc) {
    if (strcmp(argv[i], "create-admin") == 0 && argc - i == 3) admin = 1;
    else return usage();
  }

  app_t *a = calloc(1, sizeof *a);
  if (!a) { fprintf(stderr, "ocs-server: out of memory\n"); return 1; }
  if (config_load(conf, &a->cfg) != 0) {
    fprintf(stderr, "ocs-server: cannot load config %s\n", conf);
    free(a);
    return 1;
  }
  if (platform_init() != 0) { fprintf(stderr, "ocs-server: platform init failed\n"); free(a); return 1; }
  if (db_load(&a->db, a->cfg.data_dir) != 0) {
    fprintf(stderr, "ocs-server: cannot load data from %s\n", a->cfg.data_dir);
    free(a);
    return 1;
  }
  a->mail_send = mail_cb;
  a->mail_ctx = a;

  int rc;
  if (admin) {
    rc = create_admin(a, argv[i + 1], argv[i + 2]);
  } else {
    rc = server_run(a) == 0 ? 0 : 1;
  }
  db_free(&a->db);
  free(a);
  return rc;
}
