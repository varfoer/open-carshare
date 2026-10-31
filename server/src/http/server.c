/* Mongoose adapter: maps HTTP connections to api_handle / static files. */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "http/api.h"
#include "mongoose.h"
#include "platform/platform.h"

#define MAX_BODY 65536
#define PRUNE_INTERVAL_MS (3600ULL * 1000ULL)
#define EXPIRE_INTERVAL_MS (300ULL * 1000ULL)

#define SEC_HEADERS \
  "Content-Security-Policy: default-src 'self'; img-src 'self' data:\r\n" \
  "X-Content-Type-Options: nosniff\r\n" \
  "Referrer-Policy: same-origin\r\n"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static void reply_error(struct mg_connection *c, int status, const char *code, const char *msg_de) {
  mg_http_reply(c, status,
                "Content-Type: application/json\r\nCache-Control: no-store\r\n" SEC_HEADERS,
                "{\"error\":\"%s\",\"message\":\"%s\"}", code, msg_de);
}

#define REJECTED(c) ((c)->data[0] == 'R')

static void reply_too_large(struct mg_connection *c) {
  c->data[0] = 'R'; /* answered already; the later MSG event must not reply again */
  reply_error(c, 413, "too_large", "Die Anfrage ist zu groß.");
  c->is_draining = 1;
}

static void copy_str(char *dst, size_t cap, struct mg_str s) {
  size_t n = s.len < cap - 1 ? s.len : cap - 1;
  if (n > 0) memcpy(dst, s.buf, n);
  dst[n] = 0;
}

static void peer_ip(const struct mg_connection *c, char *out, size_t cap) {
  /* mg_print_ip via "%M" renders IPv4 dotted or IPv6 text */
  mg_snprintf(out, cap, "%M", mg_print_ip, &c->rem);
}

/* First entry of X-Forwarded-For, trimmed; false if absent or unusable. */
static bool first_forwarded(struct mg_http_message *hm, char *out, size_t cap) {
  struct mg_str *xff = mg_http_get_header(hm, "X-Forwarded-For");
  if (!xff || xff->len == 0) return false;
  size_t n = 0;
  while (n < xff->len && xff->buf[n] != ',') n++;
  const char *b = xff->buf;
  while (n > 0 && (*b == ' ' || *b == '\t')) { b++; n--; }
  while (n > 0 && (b[n - 1] == ' ' || b[n - 1] == '\t')) n--;
  if (n == 0 || n >= cap) return false;
  memcpy(out, b, n);
  out[n] = 0;
  return true;
}

static void handle_api(struct mg_connection *c, struct mg_http_message *hm, app_t *a) {
  char method[16], path[2048], query[2048], ctype[128], token[128] = "", ip[64];
  if (hm->method.len >= sizeof method || hm->uri.len >= sizeof path || hm->query.len >= sizeof query) {
    reply_error(c, 400, "bad_request", "Ungültige Anfrage.");
    return;
  }
  copy_str(method, sizeof method, hm->method);
  copy_str(path, sizeof path, hm->uri);
  copy_str(query, sizeof query, hm->query);

  ctype[0] = 0;
  struct mg_str *ct = mg_http_get_header(hm, "Content-Type");
  if (ct) copy_str(ctype, sizeof ctype, *ct);

  bool have_token = false;
  struct mg_str *cookie = mg_http_get_header(hm, "Cookie");
  if (cookie) {
    struct mg_str v = mg_http_get_header_var(*cookie, mg_str("ocs_session"));
    if (v.len > 0 && v.len < sizeof token) { copy_str(token, sizeof token, v); have_token = true; }
  }

  peer_ip(c, ip, sizeof ip);
  if (strcmp(ip, "127.0.0.1") == 0) {
    char fwd[64];
    if (first_forwarded(hm, fwd, sizeof fwd)) memcpy(ip, fwd, strlen(fwd) + 1);
  }

  api_req_t req = {
    .method = method, .path = path, .query = query, .body = hm->body.buf,
    .content_type = ct ? ctype : NULL, .session_token = have_token ? token : NULL,
    .client_ip = ip, .body_len = hm->body.len, .now = platform_now(),
  };
  api_res_t res;
  memset(&res, 0, sizeof res);
  api_handle(a, &req, &res);
  if (!res.body) {
    reply_error(c, 500, "internal", "Interner Fehler.");
  } else {
    char hdr[768];
    snprintf(hdr, sizeof hdr, "Content-Type: application/json\r\nCache-Control: no-store\r\n" SEC_HEADERS "%s%s%s",
             res.set_cookie[0] ? "Set-Cookie: " : "", res.set_cookie, res.set_cookie[0] ? "\r\n" : "");
    mg_http_reply(c, res.status, hdr, "%s", res.body);
  }
  api_res_free(&res);
}

static bool is_api_path(struct mg_str uri) {
  return (uri.len == 4 && memcmp(uri.buf, "/api", 4) == 0) ||
         (uri.len >= 5 && memcmp(uri.buf, "/api/", 5) == 0);
}

static void reply_plain(struct mg_connection *c, int status, const char *text) {
  mg_http_reply(c, status, "Content-Type: text/plain; charset=utf-8\r\n" SEC_HEADERS, "%s\n", text);
}

/* Static files. Every outcome that mongoose would answer without our headers
 * (400 bad path, 301 directory redirect, 403/404 listing) is decided here first,
 * so only a plain existing file reaches mg_http_serve_dir. */
static void serve_static(struct mg_connection *c, struct mg_http_message *hm, app_t *a) {
  char dec[2048], path[sizeof a->cfg.web_dir + sizeof dec + 16];
  int dn = mg_url_decode(hm->uri.buf, hm->uri.len, dec, sizeof dec, 0);
  if (dn < 0 || (size_t)dn != strlen(dec) || !mg_path_is_sane(mg_str_n(dec, (size_t)dn))) {
    reply_plain(c, 400, "Ungültiger Pfad.");
    return;
  }
  snprintf(path, sizeof path, "%s/%s", a->cfg.web_dir, dec);
  int fl = mg_fs_posix.st(path, NULL, NULL);
  bool is_dir = (fl & MG_FS_DIR) != 0;
  bool slash = hm->uri.len > 0 && hm->uri.buf[hm->uri.len - 1] == '/';
  if (fl == 0) {
    reply_plain(c, 404, "Nicht gefunden.");
    return;
  }
  if (is_dir && !slash) {
    char hdr[2048 + 128];
    snprintf(hdr, sizeof hdr, "Location: %.*s/\r\n" SEC_HEADERS, (int)hm->uri.len, hm->uri.buf);
    mg_http_reply(c, 301, hdr, "");
    return;
  }
  if (is_dir) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = 0;
    snprintf(path + n, sizeof path - n, "/index.html");
    int ifl = mg_fs_posix.st(path, NULL, NULL);
    if (ifl == 0 || (ifl & MG_FS_DIR)) {
      reply_plain(c, 404, "Nicht gefunden.");
      return;
    }
  }
  struct mg_http_serve_opts opts;
  memset(&opts, 0, sizeof opts);
  opts.root_dir = a->cfg.web_dir;
  opts.extra_headers = SEC_HEADERS;
  mg_http_serve_dir(c, hm, &opts);
}

static void on_event(struct mg_connection *c, int ev, void *ev_data) {
  app_t *a = c->fn_data;
  if (ev == MG_EV_HTTP_HDRS) {
    struct mg_http_message *hm = ev_data;
    if (hm->body.len > MAX_BODY) reply_too_large(c);
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = ev_data;
    if (REJECTED(c)) {
      /* 413 already sent from HDRS */
    } else if (hm->body.len > MAX_BODY) {
      reply_too_large(c);
    } else if (is_api_path(hm->uri)) {
      handle_api(c, hm, a);
    } else {
      serve_static(c, hm, a);
    }
  }
}

static void prune_cb(void *arg) {
  app_t *a = arg;
  if (db_prune(&a->db, platform_now()) != 0) fprintf(stderr, "ocs-server: db_prune failed\n");
}

static void expire_cb(void *arg) {
  app_t *a = arg;
  if (api_expire_bookings(a, platform_now()) < 0) fprintf(stderr, "ocs-server: api_expire_bookings failed\n");
}

int server_run(app_t *a) {
  g_stop = 0;
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  mg_log_set(MG_LL_ERROR);
  struct mg_mgr mgr;
  mg_mgr_init(&mgr);
  if (!mg_http_listen(&mgr, a->cfg.listen, on_event, a)) {
    fprintf(stderr, "ocs-server: cannot listen on %s\n", a->cfg.listen);
    mg_mgr_free(&mgr);
    return -1;
  }
  mg_timer_add(&mgr, PRUNE_INTERVAL_MS, MG_TIMER_REPEAT | MG_TIMER_RUN_NOW, prune_cb, a);
  mg_timer_add(&mgr, EXPIRE_INTERVAL_MS, MG_TIMER_REPEAT | MG_TIMER_RUN_NOW, expire_cb, a);
  while (!g_stop) mg_mgr_poll(&mgr, 200);
  mg_mgr_free(&mgr);
  return 0;
}
