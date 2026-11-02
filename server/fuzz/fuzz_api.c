/* libFuzzer target for api_handle.
 * Input: byte 0 route (POST/PUT/DELETE, modulo count), byte 1 session
 * (0 none, 1 user, 2 admin), byte 2 id selector (ids 1-3 for each '*'), rest body
 * (sent as image/jpeg on JPEG upload routes, else application/json). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth/session.h"
#include "helpers.h"

#define FUZZ_NOW INT64_C(1800000000)
#define RECREATE_EVERY 2000

static app_t *app;
static char user_tok[65], admin_tok[65];
static size_t routes[64], nroutes;
static unsigned iters;

static void teardown(void) {
  th_free(app);
  app = NULL;
}

static int setup(void) {
  app = th_app();
  if (!app) return -1;
  /* Admin: empty pw_hash + session_create, no argon2. The user gets a password
   * so DELETE /api/me/mandate gets past the password check; th_set_creditor
   * makes POST /api/me/mandate reachable. */
  int64_t aid = th_add_user(app, "admin@fuzz.test", NULL, ROLE_ADMIN);
  int64_t uid = th_add_user(app, "user@fuzz.test", "fuzz password 1", ROLE_USER);
  if (aid < 0 || uid < 0 || th_set_creditor(app) != 0 || session_create(app, aid, FUZZ_NOW, admin_tok) != 0 ||
      session_create(app, uid, FUZZ_NOW, user_tok) != 0)
    return -1;
  /* One active car (id 1) so booking writes get past BR_INVALID. */
  car_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "Fuzz");
  snprintf(c.plate, sizeof c.plate, "F-1");
  snprintf(c.location, sizeof c.location, "Hof");
  c.active = true;
  if (db_put_car(&app->db, &c) != 0 || c.id != 1) return -1;
  return 0;
}

/* Whatever the API wrote must load back: a data dir db_load rejects would
 * keep the server from restarting. */
static void check_dir_loads(void) {
  db_t d;
  if (db_load(&d, app->cfg.data_dir) != 0) {
    fprintf(stderr, "fuzz_api: db_load failed on %s\n", app->cfg.data_dir);
    abort();
  }
  db_free(&d);
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  setenv("TMPDIR", "/dev/shm", 1);
  for (size_t i = 0; i < api_route_count() && nroutes < 64; i++)
    if (strcmp(api_route_method(i), "GET") != 0) routes[nroutes++] = i;
  if (nroutes == 0 || setup() != 0) {
    fprintf(stderr, "fuzz_api: setup failed\n");
    exit(1);
  }
  return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 3) return 0;
  /* Fresh app periodically (bounds data growth) and whenever a session died
   * (logout, deactivation, password change). */
  if (++iters % RECREATE_EVERY == 0 || !session_user(app, user_tok, FUZZ_NOW) ||
      !session_user(app, admin_tok, FUZZ_NOW)) {
    check_dir_loads();
    teardown();
    if (setup() != 0) {
      fprintf(stderr, "fuzz_api: re-setup failed\n");
      abort();
    }
  }

  size_t ri = routes[data[0] % nroutes];
  const char *tok = data[1] % 3 == 0 ? NULL : data[1] % 3 == 1 ? user_tok : admin_tok;
  unsigned idsel = data[2];

  char path[128];
  const char *pat = api_route_pattern(ri);
  size_t n = 0;
  for (; *pat && n + 2 < sizeof path; pat++) {
    if (*pat == '*') {
      path[n++] = (char)('1' + idsel % 3);
      idsel /= 3;
    } else {
      path[n++] = *pat;
    }
  }
  path[n] = 0;

  /* Exact-size heap copy so ASan catches reads past body_len. */
  size_t blen = size - 3;
  char *body = malloc(blen ? blen : 1);
  if (!body) abort();
  if (blen) memcpy(body, data + 3, blen);

  api_req_t req;
  memset(&req, 0, sizeof req);
  req.method = api_route_method(ri);
  req.path = path;
  req.query = NULL;
  req.body = body;
  req.body_len = blen;
  /* Only BODY_JPEG routes have the larger limit; they take the input as image/jpeg. */
  req.content_type = api_body_limit(req.method, path) > 65536 ? "image/jpeg" : "application/json";
  req.session_token = tok;
  req.client_ip = "1.2.3.4";
  req.now = FUZZ_NOW;
  api_res_t res;
  api_handle(app, &req, &res);
  int s = res.status;
  api_res_free(&res);
  free(body);
  switch (s) {
    case 200: case 201: case 400: case 401: case 403: case 404:
    case 405: case 409: case 413: case 415: case 429:
      break;
    default:
      fprintf(stderr, "fuzz_api: unexpected status %d on %s %s\n", s, req.method, path);
      abort();
  }
  return 0;
}
