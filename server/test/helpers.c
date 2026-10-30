#include "helpers.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "auth/password.h"
#include "cJSON.h"
#include "platform/platform.h"

/* ---- fake mailer ---- */

static char last_to[256], last_subject[256], last_body[8192];
static int mail_count, mail_fail;

static int fake_mail(void *ctx, const char *to, const char *subject, const char *body) {
  (void)ctx;
  if (mail_fail) return -1;
  snprintf(last_to, sizeof last_to, "%s", to ? to : "");
  snprintf(last_subject, sizeof last_subject, "%s", subject ? subject : "");
  snprintf(last_body, sizeof last_body, "%s", body ? body : "");
  mail_count++;
  return 0;
}

const char *th_last_mail_to(void) { return last_to; }
const char *th_last_mail_subject(void) { return last_subject; }
const char *th_last_mail_body(void) { return last_body; }
int th_mail_count(void) { return mail_count; }
void th_mail_fail(int fail) { mail_fail = fail; }

/* ---- app ---- */

static void rm_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    char path[512];
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
      unlink(path);
    }
    closedir(d);
  }
  rmdir(dir);
}

app_t *th_app(void) {
  app_t *a = calloc(1, sizeof *a);
  if (!a) return NULL;
  const char *base = getenv("TMPDIR");
  int n = snprintf(a->cfg.data_dir, sizeof a->cfg.data_dir, "%s/ocs_app_XXXXXX",
                   base && *base ? base : "/tmp");
  if (n < 0 || (size_t)n >= sizeof a->cfg.data_dir || !mkdtemp(a->cfg.data_dir)) {
    free(a);
    return NULL;
  }
  if (db_load(&a->db, a->cfg.data_dir) != 0) {
    rm_dir(a->cfg.data_dir);
    free(a);
    return NULL;
  }
  snprintf(a->cfg.base_url, sizeof a->cfg.base_url, "%s", TH_BASE_URL);
  snprintf(a->cfg.mail_from, sizeof a->cfg.mail_from, "noreply@carshare.test");
  a->mail_send = fake_mail;
  a->mail_ctx = NULL;
  mail_count = 0;
  mail_fail = 0;
  last_to[0] = last_subject[0] = last_body[0] = 0;
  return a;
}

void th_free(app_t *a) {
  if (!a) return;
  db_free(&a->db);
  rm_dir(a->cfg.data_dir);
  free(a);
}

int64_t th_add_user(app_t *a, const char *email, const char *pw, role_t role) {
  user_t u;
  memset(&u, 0, sizeof u);
  if (!a || !email || strlen(email) >= sizeof u.email) return -1;
  snprintf(u.email, sizeof u.email, "%s", email);
  snprintf(u.name, sizeof u.name, "Test User");
  u.role = role;
  u.active = true;
  if (pw && *pw && pw_hash(pw, u.pw_hash) != 0) return -1;
  if (db_put_user(&a->db, &u) != 0) return -1;
  return u.id;
}

/* ---- requests ---- */

static api_res_t req_full(app_t *a, const char *method, const char *path, const char *body,
                          const char *token, int64_t now, const char *content_type,
                          const char *ip) {
  api_res_t res;
  memset(&res, 0, sizeof res);
  size_t pl = path ? strlen(path) : 0;
  char *p = malloc(pl + 1);
  if (!p) return res;
  if (path) memcpy(p, path, pl);
  p[pl] = 0;
  char *q = strchr(p, '?');
  if (q) *q++ = 0;
  api_req_t req;
  memset(&req, 0, sizeof req);
  req.method = method;
  req.path = path ? p : NULL;
  req.query = q;
  req.body = body;
  req.body_len = body ? strlen(body) : 0;
  req.content_type = content_type;
  req.session_token = token;
  req.client_ip = ip;
  req.now = now;
  api_handle(a, &req, &res);
  free(p);
  return res;
}

api_res_t th_req_ct(app_t *a, const char *method, const char *path, const char *body,
                    const char *token, int64_t now, const char *content_type) {
  return req_full(a, method, path, body, token, now, content_type, "1.2.3.4");
}

api_res_t th_req_ip(app_t *a, const char *method, const char *path, const char *body,
                    int64_t now, const char *ip) {
  return req_full(a, method, path, body, NULL, now, "application/json", ip);
}

api_res_t th_req(app_t *a, const char *method, const char *path, const char *body,
                 const char *token, int64_t now) {
  return th_req_ct(a, method, path, body, token, now, "application/json");
}

char *th_login_at(app_t *a, const char *email, const char *pw, int64_t now) {
  static char tok[65];
  cJSON *j = cJSON_CreateObject();
  if (!j || !cJSON_AddStringToObject(j, "email", email ? email : "") ||
      !cJSON_AddStringToObject(j, "password", pw ? pw : "")) {
    cJSON_Delete(j);
    return NULL;
  }
  char *body = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  if (!body) return NULL;
  api_res_t r = th_req(a, "POST", "/api/login", body, NULL, now);
  free(body);
  char *ret = NULL;
  static const char pre[] = "ocs_session=";
  if (r.status == 200 && strncmp(r.set_cookie, pre, sizeof pre - 1) == 0) {
    const char *t = r.set_cookie + sizeof pre - 1;
    size_t n = strcspn(t, ";");
    if (n == 64) {
      memcpy(tok, t, 64);
      tok[64] = 0;
      ret = tok;
    }
  }
  api_res_free(&r);
  return ret;
}

char *th_login(app_t *a, const char *email, const char *pw) {
  return th_login_at(a, email, pw, platform_now());
}
