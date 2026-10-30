#ifndef OCS_TEST_HELPERS_H
#define OCS_TEST_HELPERS_H

/* Test helpers. No Unity asserts in here, so a libFuzzer harness can use them:
 * failures are reported as NULL / -1. */

#include <stdint.h>

#include "app.h"
#include "core/model.h"
#include "http/api.h"

#define TH_BASE_URL "https://carshare.test"

app_t *th_app(void);                       /* fresh mkdtemp data dir, fake mailer; NULL on failure */
void th_free(app_t *a);                    /* frees app and removes the data dir */
const char *th_last_mail_to(void);
const char *th_last_mail_subject(void);
const char *th_last_mail_body(void);
int th_mail_count(void);                   /* reset by th_app */
void th_mail_fail(int fail);               /* nonzero: fake mailer returns -1 */
/* Active user, name "Test User"; pw NULL or "" -> empty pw_hash. id or -1. */
int64_t th_add_user(app_t *a, const char *email, const char *pw, role_t role);
/* Logs in via POST /api/login at platform_now(); token (static buf) or NULL. */
char *th_login(app_t *a, const char *email, const char *pw);
char *th_login_at(app_t *a, const char *email, const char *pw, int64_t now);
/* path may contain "?query". content_type application/json, ip "1.2.3.4",
 * body_len = strlen(body) (NULL body ok). Caller frees with api_res_free. */
api_res_t th_req(app_t *a, const char *method, const char *path, const char *body,
                 const char *token, int64_t now);
/* Like th_req with explicit content type (NULL allowed). */
api_res_t th_req_ct(app_t *a, const char *method, const char *path, const char *body,
                    const char *token, int64_t now, const char *content_type);
/* Unauthenticated JSON request from client ip. */
api_res_t th_req_ip(app_t *a, const char *method, const char *path, const char *body,
                    int64_t now, const char *ip);

#endif
