#ifndef OCS_TEST_HELPERS_H
#define OCS_TEST_HELPERS_H

/* Test helpers. No Unity asserts in here, so a libFuzzer harness can use them:
 * failures are reported as NULL / -1. */

#include <stddef.h>
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
const char *th_last_mail_att_name(void);  /* "" if the last mail had no attachment */
size_t th_last_mail_att_len(void);         /* 0 if none */
int th_mail_count(void);                   /* reset by th_app */
void th_mail_fail(int fail);               /* nonzero: fake mailer returns -1 */
void th_mail_fail_to(const char *to);      /* fake mailer fails only for this recipient; NULL = off; reset by th_app */
/* Fake igloohome PIN backend, wired and reset by th_app. Each call counts; on success the PIN is
 * "10000<count>" (e.g. "100001") and the pinId "P<count>". */
void th_pin_fail(pin_err_t e);             /* PIN_OK = succeed */
void th_pin_malformed(int on);             /* nonzero: answer as if igloohome sent garbage (PIN_ERR_OTHER) */
int th_pin_count(void);                    /* calls so far, failed ones included */
const char *th_pin_last_device(void);
int64_t th_pin_last_from(void);
int64_t th_pin_last_until(void);
int th_pin_last_variance(void);
const char *th_pin_last_name(void);
/* Fake igloohome device list: th_locks_set replaces the list, th_locks_fail makes it fail with e
 * (PIN_OK = succeed). Both reset by th_app. */
void th_locks_set(const lock_info_t *l, size_t n);
void th_locks_fail(pin_err_t e);
/* Fake push services (a->https), wired and reset by th_app, which also sets up the VAPID key.
 * Every request answers th_push_status (default 201; -1 = transport error); requests whose URL
 * contains the th_push_status_for needle answer that status instead (needle NULL/"" = off). */
void th_push_status(int status);
void th_push_status_for(const char *needle, int status);
int th_push_count(void);
const char *th_push_last_url(void);
const char *th_push_last_auth(void);       /* the Authorization header line */
size_t th_push_last_len(void);             /* body bytes */
/* Browser key of RFC 8291 section 5 (valid point) and its auth secret. */
#define TH_PUSH_P256DH "BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4"
#define TH_PUSH_AUTH "BTBZMqHH6r4Tts7J_aSIgg"
/* Subscription with the key above; id or -1. */
int64_t th_add_push_sub(app_t *a, int64_t user_id, const char *endpoint);
/* Active user, name "Test User"; pw NULL or "" -> empty pw_hash. id or -1.
 * th_add_user also inserts an ACTIVE mandate (holder "Test User", IBAN DE89370400440532013000,
 * text_version 1); th_add_user_bare does not. */
int64_t th_add_user_bare(app_t *a, const char *email, const char *pw, role_t role);
int th_set_creditor(app_t *a); /* 0 ok, else -1; assoc_name "Carsharing Testverein e.V.", creditor_id "DE98ZZZ09999999999" */
int64_t th_add_user(app_t *a, const char *email, const char *pw, role_t role);
/* Named tariff without versions; id or -1. */
int64_t th_add_tariff(app_t *a, const char *name);
/* One version with the standard rates (300/150/30, night 1200-360, unused 500) and the given
 * caps/late fee; created_at = valid_from. id or -1. */
int64_t th_add_version(app_t *a, int64_t tariff_id, int64_t valid_from, int64_t time_cap,
                       int64_t combined_cap, int64_t late_fee);
/* Assigns tariff_id to user_id from valid_from, created_by 0; id or -1. */
int64_t th_assign_tariff(app_t *a, int64_t user_id, int64_t tariff_id, int64_t valid_from);
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
/* Binary body of len bytes with explicit content type (NULL allowed). */
api_res_t th_req_bin(app_t *a, const char *method, const char *path, const void *body, size_t len,
                     const char *token, int64_t now, const char *content_type);

/* Valid Codec 8E packet with n records (zero GPS), each carrying only IO 16 =
 * odo_m[i] as N4 (odo_m[i] < 0: no I/O at all). Returns length, 0 if it does not fit. */
size_t th_tlt_packet(uint8_t *out, size_t cap, const int64_t *ts_ms, const int64_t *odo_m, size_t n);

#endif
