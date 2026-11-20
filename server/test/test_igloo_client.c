#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "http/igloo_client.h"

#define T0 1774742400
#define W 1795179600
#define H 3600
#define MAXQ 8

typedef struct { int rc, status; const char *body; } ans_t;
typedef struct { char method[8], url[256], bearer[64], body[512]; long timeout_s; } seen_t;

static ans_t queue[MAXQ];
static size_t nq, qi;
static seen_t seen[MAXQ];
static size_t nseen;
static int64_t clk, step;

static int64_t fake_now(void) { return clk; }

static int fake_http(const http_req_t *r, int *status, char *out, size_t cap) {
  if (nseen < MAXQ) {
    seen_t *s = &seen[nseen];
    snprintf(s->method, sizeof s->method, "%s", r->method);
    snprintf(s->url, sizeof s->url, "%s", r->url);
    snprintf(s->bearer, sizeof s->bearer, "%s", r->bearer ? r->bearer : "");
    snprintf(s->body, sizeof s->body, "%s", r->body ? r->body : "");
    s->timeout_s = r->timeout_s;
  }
  nseen++;
  clk += step;
  if (qi >= nq) return -1;
  ans_t a = queue[qi++];
  if (a.rc != 0) return -1;
  *status = a.status;
  snprintf(out, cap, "%s", a.body);
  return 0;
}

static config_t cfg;
static igloo_client_t cl;

void setUp(void) {
  memset(&cfg, 0, sizeof cfg);
  snprintf(cfg.igloo_client_id, sizeof cfg.igloo_client_id, "cid");
  snprintf(cfg.igloo_client_secret, sizeof cfg.igloo_client_secret, "sec");
  snprintf(cfg.igloo_api_url, sizeof cfg.igloo_api_url, "https://api.igloohome.co");
  snprintf(cfg.igloo_token_url, sizeof cfg.igloo_token_url, "https://auth.igloohome.co/oauth2/token");
  nq = qi = nseen = 0;
  clk = T0;
  step = 0;
  igloo_client_init(&cl, &cfg, fake_http, fake_now);
}
void tearDown(void) {}

static void q(int status, const char *body) { queue[nq++] = (ans_t){0, status, body}; }
static void qfail(void) { queue[nq++] = (ans_t){-1, 0, ""}; }
#define TOK "{\"access_token\":\"tok\",\"expires_in\":86400}"
#define PINOK "{\"pin\":\"5729314\",\"pinId\":\"AA98\"}"

static pin_err_t create(pin_result_t *r) {
  memset(r, 0x7f, sizeof *r);
  r->pin[0] = 0;
  return igloo_pin_create(&cl, "D1", W, W + 4 * H, 1, "Buchung 1", r);
}

static void test_pin_create_ok(void) {
  pin_result_t r;
  q(200, TOK); q(200, PINOK);
  TEST_ASSERT_EQUAL_INT(PIN_OK, create(&r));
  TEST_ASSERT_EQUAL_STRING("5729314", r.pin);
  TEST_ASSERT_EQUAL_STRING("AA98", r.pin_id);
  TEST_ASSERT_EQUAL_UINT(2, nseen);
  TEST_ASSERT_EQUAL_STRING("POST", seen[0].method);
  TEST_ASSERT_EQUAL_STRING("https://auth.igloohome.co/oauth2/token", seen[0].url);
  TEST_ASSERT_EQUAL_STRING("grant_type=client_credentials", seen[0].body);
  TEST_ASSERT_EQUAL_STRING("POST", seen[1].method);
  TEST_ASSERT_EQUAL_STRING("https://api.igloohome.co/home/devices/D1/algopin/hourly", seen[1].url);
  TEST_ASSERT_EQUAL_STRING("tok", seen[1].bearer);
  TEST_ASSERT_NOT_NULL(strstr(seen[1].body, "\"variance\":1"));
}

static void test_token_cached(void) {
  pin_result_t r;
  q(200, TOK); q(200, PINOK); q(200, PINOK);
  TEST_ASSERT_EQUAL_INT(PIN_OK, create(&r));
  TEST_ASSERT_EQUAL_INT(PIN_OK, create(&r));
  TEST_ASSERT_EQUAL_UINT(3, nseen);
  clk = T0 + 86400 - 301;
  q(200, PINOK);
  TEST_ASSERT_EQUAL_INT(PIN_OK, create(&r));
  TEST_ASSERT_EQUAL_UINT(4, nseen);
  clk = T0 + 86400 - 299;
  q(200, TOK); q(200, PINOK);
  TEST_ASSERT_EQUAL_INT(PIN_OK, create(&r));
  TEST_ASSERT_EQUAL_UINT(6, nseen);
  TEST_ASSERT_EQUAL_STRING("https://auth.igloohome.co/oauth2/token", seen[4].url);
}

static void test_401_retries_once(void) {
  pin_result_t r;
  q(200, TOK); q(401, ""); q(200, TOK); q(200, PINOK);
  TEST_ASSERT_EQUAL_INT(PIN_OK, create(&r));
  TEST_ASSERT_EQUAL_UINT(4, nseen);
  nq = qi = nseen = 0;
  q(200, TOK); q(401, ""); q(200, TOK); q(401, "");
  igloo_client_init(&cl, &cfg, fake_http, fake_now);
  TEST_ASSERT_EQUAL_INT(PIN_ERR_AUTH, create(&r));
  TEST_ASSERT_EQUAL_UINT(4, nseen);
  TEST_ASSERT_EQUAL_INT(401, r.http_status);
}

static pin_err_t one_status(int status, const char *body, pin_result_t *r) {
  nq = qi = nseen = 0;
  igloo_client_init(&cl, &cfg, fake_http, fake_now);
  q(200, TOK); q(status, body);
  return create(r);
}

static void test_status_mapping(void) {
  pin_result_t r;
  TEST_ASSERT_EQUAL_INT(PIN_ERR_PAYMENT, one_status(402, "", &r));
  TEST_ASSERT_EQUAL_INT(PIN_ERR_SCOPE, one_status(403, "", &r));
  TEST_ASSERT_EQUAL_INT(PIN_ERR_DEVICE, one_status(404, "", &r));
  TEST_ASSERT_EQUAL_INT(PIN_ERR_TIMEZONE, one_status(409, "", &r));
  TEST_ASSERT_EQUAL_INT(PIN_ERR_UNSUPPORTED, one_status(418, "", &r));
  TEST_ASSERT_EQUAL_INT(PIN_ERR_OTHER, one_status(500, "", &r));
  TEST_ASSERT_EQUAL_INT(500, r.http_status);
  TEST_ASSERT_EQUAL_INT(PIN_ERR_OTHER, one_status(200, "not json", &r));
  /* token endpoint */
  for (int st = 400; st <= 401; st++) {
    nq = qi = nseen = 0;
    igloo_client_init(&cl, &cfg, fake_http, fake_now);
    q(st, "");
    TEST_ASSERT_EQUAL_INT(PIN_ERR_AUTH, create(&r));
    TEST_ASSERT_EQUAL_UINT(1, nseen);
  }
  nq = qi = nseen = 0;
  igloo_client_init(&cl, &cfg, fake_http, fake_now);
  q(200, "{}");
  TEST_ASSERT_EQUAL_INT(PIN_ERR_OTHER, create(&r));
  /* transport */
  nq = qi = nseen = 0;
  igloo_client_init(&cl, &cfg, fake_http, fake_now);
  q(200, TOK); qfail();
  TEST_ASSERT_EQUAL_INT(PIN_ERR_NETWORK, create(&r));
  TEST_ASSERT_EQUAL_INT(0, r.http_status);
  nq = qi = nseen = 0;
  igloo_client_init(&cl, &cfg, fake_http, fake_now);
  qfail();
  TEST_ASSERT_EQUAL_INT(PIN_ERR_NETWORK, create(&r));
}

static void test_not_configured(void) {
  pin_result_t r;
  cfg.igloo_client_id[0] = 0;
  TEST_ASSERT_EQUAL_INT(PIN_ERR_CONFIG, create(&r));
  TEST_ASSERT_EQUAL_UINT(0, nseen);
  lock_info_t l[2];
  size_t n;
  int st;
  TEST_ASSERT_EQUAL_INT(PIN_ERR_CONFIG, igloo_lock_list(&cl, l, 2, &n, &st));
  TEST_ASSERT_EQUAL_UINT(0, nseen);
}

static void test_budget(void) {
  pin_result_t r;
  step = 9;
  q(200, TOK); q(401, ""); q(200, TOK);
  TEST_ASSERT_EQUAL_INT(PIN_ERR_NETWORK, create(&r));
  TEST_ASSERT_EQUAL_UINT(2, nseen);
  TEST_ASSERT_EQUAL_INT(15, seen[0].timeout_s);
  TEST_ASSERT_EQUAL_INT(6, seen[1].timeout_s);
}

static void test_lock_list(void) {
  lock_info_t l[4];
  size_t n = 99;
  int st = 0;
  q(200, TOK);
  q(200, "{\"payload\":[{\"deviceId\":\"IGK1\",\"deviceName\":\"Box A\"},{\"deviceId\":\"IGK2\"}]}");
  TEST_ASSERT_EQUAL_INT(PIN_OK, igloo_lock_list(&cl, l, 4, &n, &st));
  TEST_ASSERT_EQUAL_UINT(2, n);
  TEST_ASSERT_EQUAL_INT(200, st);
  TEST_ASSERT_EQUAL_STRING("IGK1", l[0].device_id);
  TEST_ASSERT_EQUAL_STRING("Box A", l[0].name);
  TEST_ASSERT_EQUAL_STRING("GET", seen[1].method);
  TEST_ASSERT_EQUAL_STRING("https://api.igloohome.co/home/devices", seen[1].url);
  TEST_ASSERT_EQUAL_STRING("tok", seen[1].bearer);
  nq = qi = nseen = 0;
  q(402, "");
  TEST_ASSERT_EQUAL_INT(PIN_ERR_PAYMENT, igloo_lock_list(&cl, l, 4, &n, &st));
  TEST_ASSERT_EQUAL_INT(402, st);
  /* 401 retry on list */
  nq = qi = nseen = 0;
  q(401, ""); q(200, TOK); q(200, "[]");
  TEST_ASSERT_EQUAL_INT(PIN_OK, igloo_lock_list(&cl, l, 4, &n, &st));
  TEST_ASSERT_EQUAL_UINT(0, n);
  TEST_ASSERT_EQUAL_UINT(3, nseen);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_pin_create_ok);
  RUN_TEST(test_token_cached);
  RUN_TEST(test_401_retries_once);
  RUN_TEST(test_status_mapping);
  RUN_TEST(test_not_configured);
  RUN_TEST(test_budget);
  RUN_TEST(test_lock_list);
  return UNITY_END();
}
