# Phase 1: Booking Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A running VPS deployment where admins manage users and cars, and users log in, see the car calendar, and create, extend and cancel bookings through a German web app installable as a PWA.

**Architecture:** One C11 binary `ocs-server`. Mongoose is only an adapter (`http/server.c`). All request logic goes through a pure function `api_handle(app_t*, const api_req_t*, api_res_t*)`, which tests call directly. Data lives in RAM (`db_t`) and is persisted per table as JSON lines with atomic writes. The web client is vanilla ES modules with no build step, served by the same binary behind Caddy.

**Tech Stack:** C11, CMake ≥ 3.22, Mongoose 7.14, cJSON 1.7.18, Monocypher 4.0.2, Unity 2.6.0, libcurl (SMTP), vanilla JS (ES2020), `node --test` for JS unit tests, Caddy, systemd.

**Spec:** `docs/spec.md`

**Out of scope (later phases):** start/return, km/Geotab, damage reports, PIN, booking history, tariffs/pricing, mandate/IBAN, billing, SEPA, PDF, booking notification mails, backups by mail.

## Global Constraints

- Time: `int64_t` Unix epoch seconds, UTC, everywhere in server and API. Display in `Europe/Berlin`, independent of the device time zone.
- Booking times must be multiples of 60 s. Intervals are half-open `[start, end)`; touching intervals do not overlap.
- Money: `int64_t` cents; no `float`/`double` in server code.
- UI text: German only. Code, identifiers, comments, docs: English.
- Fixed-size `char[]` fields in structs (portable to ESP32 later); every copy is length-checked, and over-long input is rejected with `400`, never truncated silently.
- No dependency beyond the Tech Stack list. `platform/posix/` is the only place for POSIX- or libcurl-specific code.
- Data dir files: `users.jsonl`, `cars.jsonl`, `bookings.jsonl`, `sessions.jsonl`, `tokens.jsonl`, `settings.json`.
- Settings defaults: `start_tolerance_min = 15`, `cancel_deadline_min = 0`, `cancel_fee_cents = 0`.
- Passwords: Argon2id, m = 19456 KiB, t = 2, p = 1, 16-byte salt, 32-byte hash; minimum password length 10.
- Session cookie: `ocs_session`, `HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=2592000` (30 days).
- Reset token TTL 1 h, invite token TTL 7 days, both single use.
- Login rate limit: 5 failures per 15 min per IP and per email → `429`.
- Request body limit 64 KiB → `413`. Mutating `/api` requests must have `Content-Type: application/json` → else `415`.
- Error response body: `{"error":"<code>","message":"<German text>"}`.
- License: Mongoose is GPLv2. The project must be GPL-2.0-compatible; add `LICENSE` with GPL-2.0-only.
- Build with `-Wall -Wextra -Werror`, and tests with `-fsanitize=address,undefined`.

## Review Focus

1. **Booking boundaries:** a booking ending 10:00 and one starting 10:00 on the same car must both be accepted; 09:59 end vs. 09:58 start must be rejected. Test in Task 6 `test_touching_intervals_do_not_overlap`.
2. **Disk write failure:** if persisting fails (e.g. read-only data dir), the RAM state must stay unchanged and the API must return `500`. No phantom booking may block the car. Test in Task 3 `test_put_rolls_back_on_write_failure`.
3. **Email case:** `Anna@x.de` and `anna@x.de` are the same account for login, reset and admin create (duplicate → `409`). Test in Task 3 `test_user_by_email_case_insensitive` and Task 9 `test_admin_create_duplicate_email_409`.
4. **Malformed input:** a broken JSON body, wrong field types, missing fields or a non-numeric id in the path must return `400`, never crash. Tests in Task 7 `test_malformed_json_400` and the fuzz target in Task 11.
5. **Device time zone ≠ Berlin:** a user abroad, or a phone set to another zone, must see and book Berlin times; DST-gap and DST-overlap times must map deterministically. Test in Task 12 running `TZ=America/New_York node --test`.

---

## File Structure

```
CMakeLists.txt                     root: options, warnings, sanitizers, subdirs
LICENSE
server/
  CMakeLists.txt                   ocs_core lib, ocs-server exe, tests, fuzz
  third_party/{mongoose,cjson,monocypher,unity}/   vendored, pinned
  src/
    core/model.h                   structs + enums
    core/codec.{h,c}               struct <-> cJSON
    core/booking_rules.{h,c}       pure booking checks
    core/mail_build.{h,c}          RFC 5322 message with RFC 2047 subject
    store/store.{h,c}              read lines, atomic write
    store/db.{h,c}                 in-RAM tables, lookup, put with rollback
    auth/password.{h,c}            argon2id PHC string
    auth/session.{h,c}             sessions + one-time tokens
    auth/ratelimit.{h,c}           failure counter table
    app.h                          app_t, config_t
    config.{h,c}                   key=value config file
    http/api.{h,c}                 api_req_t/api_res_t, router, helpers
    http/api_auth.c                login, logout, password, me
    http/api_booking.c             cars, calendar, bookings (user)
    http/api_admin.c               admin endpoints
    http/server.c                  Mongoose adapter
    platform/platform.h            now, random, mail send
    platform/posix/platform_posix.c
    main.c
  test/
    helpers.{h,c}                  tmp dir, test app, fake mailer, request helper
    test_codec.c test_db.c test_password.c test_session.c test_ratelimit.c
    test_mail_build.c test_booking_rules.c test_api_auth.c
    test_api_booking.c test_api_admin.c test_config.c
  fuzz/fuzz_api.c
web/
  index.html style.css manifest.json sw.js icon.svg icon-192.png icon-512.png
  js/time.js js/api.js js/app.js
  js/views/{login,password,calendar,booking_form,my_bookings,account,admin_users,admin_cars,admin_bookings,admin_settings}.js
  test/time.test.mjs
deploy/vps/{Caddyfile,ocs-server.service,ocs.conf.example,README.md}
```

---

### Task 1: Scaffold, vendored dependencies, test harness

**Files:**
- Create: `CMakeLists.txt`, `server/CMakeLists.txt`, `LICENSE`, `.gitignore` (`build/`, `data/`)
- Create: `server/third_party/*` (vendored)
- Test: `server/test/test_smoke.c`

**Interfaces:**
- Produces: CMake target `ocs_core` (static lib of all `server/src/**` except `main.c` and `http/server.c`), `ocs-server` executable, and the function `ocs_add_test(name)` in `server/CMakeLists.txt`, which builds `test/<name>.c` + `test/helpers.c` against `ocs_core` + Unity and registers it with CTest.

- [ ] **Step 1: Install system packages**

Run: `sudo apt install -y libcurl4-openssl-dev`
Expected: `curl-config --version` prints a version.

- [ ] **Step 2: Vendor dependencies at pinned tags**

```bash
cd server/third_party
mkdir -p mongoose cjson monocypher unity
B=https://raw.githubusercontent.com
curl -fsSLo mongoose/mongoose.c   $B/cesanta/mongoose/7.14/mongoose.c
curl -fsSLo mongoose/mongoose.h   $B/cesanta/mongoose/7.14/mongoose.h
curl -fsSLo cjson/cJSON.c         $B/DaveGamble/cJSON/v1.7.18/cJSON.c
curl -fsSLo cjson/cJSON.h         $B/DaveGamble/cJSON/v1.7.18/cJSON.h
curl -fsSLo monocypher/monocypher.c $B/LoupVaillant/Monocypher/4.0.2/src/monocypher.c
curl -fsSLo monocypher/monocypher.h $B/LoupVaillant/Monocypher/4.0.2/src/monocypher.h
for f in unity.c unity.h unity_internals.h; do curl -fsSLo unity/$f $B/ThrowTheSwitch/Unity/v2.6.0/src/$f; done
sha256sum */* > VERSIONS.sha256
```

Third-party sources compile without `-Werror`.

- [ ] **Step 3: Write the CMake files**

Root: `project(open_carshare C)`, `CMAKE_C_STANDARD 11`, option `OCS_SANITIZE` (default `ON`) adding `-fsanitize=address,undefined` to compile and link flags, `enable_testing()`, `add_subdirectory(server)`. `server/CMakeLists.txt`: `find_package(CURL REQUIRED)`, `ocs_core` with `-Wall -Wextra -Werror` on own sources only, `ocs_add_test`.

- [ ] **Step 4: Write a smoke test**

`test_smoke.c`: `TEST_ASSERT_EQUAL_INT(1, 1)`. Also create empty `helpers.c`/`helpers.h`; Task 7 fills them.

- [ ] **Step 5: Build and run**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 1`

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "build: scaffold cmake, vendor deps, unity harness"
```

---

### Task 2: Models and JSON codec

**Files:**
- Create: `server/src/core/model.h`, `server/src/core/codec.h`, `server/src/core/codec.c`
- Test: `server/test/test_codec.c`

**Interfaces:**
- Produces (`model.h`):

```c
typedef enum { ROLE_USER = 0, ROLE_ADMIN = 1 } role_t;
typedef enum { BK_RESERVED = 0, BK_ACTIVE, BK_DONE, BK_CANCELLED } booking_state_t;
typedef enum { TOKEN_RESET = 0, TOKEN_INVITE = 1 } token_kind_t;

typedef struct {
  int64_t id; char email[128]; char name[96]; char street[96]; char zip[16];
  char city[64]; char pw_hash[160]; role_t role; bool active;
  int64_t license_checked_at; int64_t created_at;
} user_t;
typedef struct { int64_t id; char name[64]; char plate[16]; char location[128]; bool active; } car_t;
typedef struct {
  int64_t id, car_id, user_id, booked_start, booked_end; booking_state_t state;
  int64_t cancelled_at, cancel_fee_cents, created_at, modified_at, modified_by;
} booking_t;
typedef struct { char token_hash[65]; int64_t user_id, expires_at; } session_t;
typedef struct { char token_hash[65]; int64_t user_id, expires_at; token_kind_t kind; bool used; } token_t;
typedef struct { int start_tolerance_min, cancel_deadline_min; int64_t cancel_fee_cents; } settings_t;
```

- Produces (`codec.h`): for each `X` in `user, car, booking, session, token, settings`:
  `cJSON *X_to_json(const X_t *v);` and `int X_from_json(const cJSON *j, X_t *out);` (0 ok, -1 on a missing required field, wrong type, or string too long for its field). `user_to_public_json(const user_t*)` omits `pw_hash`. JSON keys are the struct field names; enums are strings: `"user"|"admin"`, `"reserved"|"active"|"done"|"cancelled"`, `"reset"|"invite"`. Unknown keys are ignored, so later phases can add fields. `settings_from_json` fills defaults for missing keys.

- [ ] **Step 1: Write failing tests**

```c
void test_booking_roundtrip(void) {
  booking_t b = {.id=7,.car_id=1,.user_id=2,.booked_start=1774742400,.booked_end=1774746000,
                 .state=BK_RESERVED,.created_at=1};
  cJSON *j = booking_to_json(&b); booking_t r;
  TEST_ASSERT_EQUAL_INT(0, booking_from_json(j, &r));
  TEST_ASSERT_EQUAL_MEMORY(&b, &r, sizeof b);  /* zero-init both */
  TEST_ASSERT_EQUAL_STRING("reserved", cJSON_GetObjectItem(j,"state")->valuestring);
  cJSON_Delete(j);
}
void test_user_umlaut_roundtrip(void);          /* name "Jürgen Müßig" survives */
void test_user_public_json_has_no_pw_hash(void);
void test_from_json_rejects_too_long_email(void);  /* 128 chars -> -1 */
void test_from_json_rejects_wrong_type(void);       /* "id":"7" -> -1 */
void test_from_json_ignores_unknown_keys(void);
void test_settings_defaults_when_missing(void);     /* {} -> 15, 0, 0 */
```

- [ ] **Step 2: Run, expect FAIL** (`ctest --test-dir build -R test_codec`: undefined references)
- [ ] **Step 3: Implement `codec.c`** (int64 via `cJSON_CreateNumber`; reject non-integral numbers)
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** `feat(core): models and json codec`

---

### Task 3: Store and in-RAM db

**Files:**
- Create: `server/src/store/store.{h,c}`, `server/src/store/db.{h,c}`
- Test: `server/test/test_db.c`

**Interfaces:**
- Consumes: codec (Task 2).
- Produces:

```c
/* store.h */
int store_read_lines(const char *path, int (*cb)(const cJSON *obj, void *ctx), void *ctx);
  /* missing file -> 0 (empty); unparsable line or cb != 0 -> -1 */
int store_write_atomic(const char *path, const char *data, size_t len);
  /* write path.tmp, fsync, rename, fsync dir; -1 on any failure, path untouched */

/* db.h */
typedef struct {
  char dir[256];
  user_t *users; size_t n_users, cap_users;
  car_t *cars; size_t n_cars, cap_cars;
  booking_t *bookings; size_t n_bookings, cap_bookings;
  session_t *sessions; size_t n_sessions, cap_sessions;
  token_t *tokens; size_t n_tokens, cap_tokens;
  settings_t settings;
} db_t;
int  db_load(db_t *db, const char *dir);
void db_free(db_t *db);
user_t *db_user(db_t *db, int64_t id);
user_t *db_user_by_email(db_t *db, const char *email);      /* ASCII case-insensitive */
car_t *db_car(db_t *db, int64_t id);
booking_t *db_booking(db_t *db, int64_t id);
int db_put_user(db_t *db, user_t *u);        /* id==0: assign max+1 and insert; else replace. persist table. */
int db_put_car(db_t *db, car_t *c);
int db_put_booking(db_t *db, booking_t *b);
int db_put_settings(db_t *db, const settings_t *s);
int db_put_session(db_t *db, const session_t *s);
int db_delete_sessions(db_t *db, const char *token_hash, int64_t user_id); /* by hash if non-NULL, else all of user */
int db_put_token(db_t *db, const token_t *t);   /* insert or replace by token_hash */
token_t *db_token(db_t *db, const char *token_hash);
int db_prune(db_t *db, int64_t now);           /* drop expired sessions/tokens, persist */
```

All `db_put_*`/`db_delete_*` return 0 on success, and -1 if persisting failed, in which case the RAM state is restored to what it was before the call (including the id counter).

- [ ] **Step 1: Write failing tests**

```c
void test_load_empty_dir(void);                 /* all n_* == 0, settings defaults */
void test_put_assigns_ids_and_persists(void) {
  /* put 2 users -> ids 1,2; db_free; db_load same dir -> n_users==2, emails equal */
}
void test_put_replaces_existing(void);
void test_user_by_email_case_insensitive(void) {
  /* put "Anna@x.de"; db_user_by_email(&db,"anna@X.DE") != NULL */
}
void test_put_rolls_back_on_write_failure(void) {
  /* put booking id 1; chmod(dir, 0500); put new booking -> -1;
     n_bookings == 1; chmod back; put new booking -> id 2 */
}
void test_corrupt_file_fails_load(void);         /* "{" line -> db_load == -1 */
void test_prune_drops_expired_sessions(void);
```

Note: the rollback test is skipped when run as root (`geteuid()==0` → `TEST_IGNORE()`).

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement** `store.c`, then `db.c`. Each table is serialized to one buffer (one `cJSON_PrintUnformatted` per line + `\n`) and written with `store_write_atomic`. Rollback: keep a copy of the replaced element (or the old `n_*` for an insert) and restore it on failure.
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** `feat(store): jsonl store with atomic writes and in-ram db`

---

### Task 4: Platform, password hashing, rate limit

**Files:**
- Create: `server/src/platform/platform.h`, `server/src/platform/posix/platform_posix.c`, `server/src/auth/password.{h,c}`, `server/src/auth/ratelimit.{h,c}`
- Test: `server/test/test_password.c`, `server/test/test_ratelimit.c`

**Interfaces:**
- Produces:

```c
/* platform.h */
int64_t platform_now(void);                         /* time(NULL) */
int platform_random(void *buf, size_t n);           /* getrandom(); 0 ok */
int platform_mail_send(const char *smtp_url, const char *user, const char *pass,
                       const char *from, const char *to, const char *message); /* libcurl; Task 5 */

/* password.h */
int  pw_hash(const char *pw, char out[160]);        /* random salt; PHC: $argon2id$v=19$m=19456,t=2,p=1$<b64salt>$<b64hash> */
bool pw_verify(const char *pw, const char *encoded); /* constant-time compare; malformed -> false */

/* ratelimit.h */
typedef struct { char key[160]; int64_t window_start; int fails; } rl_entry_t;
typedef struct { rl_entry_t e[256]; } ratelimit_t;
bool rl_allowed(const ratelimit_t *rl, const char *key, int64_t now);  /* fails < 5 within 900 s */
void rl_fail(ratelimit_t *rl, const char *key, int64_t now);           /* full table: evict oldest window_start */
void rl_reset(ratelimit_t *rl, const char *key);
```

Base64 without padding (standard alphabet), per PHC. `pw_verify` parses `m,t,p` from the string, so the parameters can be changed later.

- [ ] **Step 1: Write failing tests**

```c
void test_hash_verify_roundtrip(void);           /* pw_verify("correct horse", h) true */
void test_wrong_password_false(void);
void test_hash_format_prefix(void);              /* starts with "$argon2id$v=19$m=19456,t=2,p=1$" */
void test_two_hashes_differ(void);               /* salt is random */
void test_malformed_encoded_false(void);         /* "", "$argon2id$", "garbage" */
void test_rl_blocks_after_5_fails(void);         /* 5x fail at t=0 -> !allowed at t=899, allowed at t=900 */
void test_rl_reset_clears(void);
void test_rl_eviction_when_full(void);           /* 257 keys: no crash, newest allowed-tracked */
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement** (`crypto_argon2` with `CRYPTO_ARGON2_ID`, work area `malloc(19456*1024)`; `crypto_verify32` for the compare. `platform_mail_send` stays a stub returning -1 until Task 5)
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** `feat(auth): argon2id password hashing and login rate limit`

---

### Task 5: Mail message builder and SMTP sending

**Files:**
- Create: `server/src/core/mail_build.{h,c}`
- Modify: `server/src/platform/posix/platform_posix.c` (implement `platform_mail_send`)
- Test: `server/test/test_mail_build.c`

**Interfaces:**
- Produces:

```c
int mail_build(char *buf, size_t n, const char *from, const char *to,
               const char *subject, const char *body, int64_t date);
/* headers: Date (RFC 5322, UTC "+0000"), From, To, Subject, MIME-Version: 1.0,
   Content-Type: text/plain; charset=UTF-8, Content-Transfer-Encoding: 8bit.
   Subject: ASCII as-is, else "=?UTF-8?B?<base64>?=". Body lines end CRLF.
   Returns length, or -1 if n is too small or to/from contain CR/LF. */
```

`platform_mail_send`: libcurl with `CURLOPT_URL=smtp_url` (e.g. `smtp://relay:587`), `CURLOPT_MAIL_FROM`, `CURLOPT_MAIL_RCPT`, `CURLOPT_READFUNCTION` streaming `message`, and a 20 s timeout. TLS is required: `CURLOPT_USE_SSL=CURLUSESSL_ALL`, except when the URL host is `localhost` or `127.0.0.1` (a local dev sink), where it is `CURLUSESSL_NONE`.

- [ ] **Step 1: Write failing tests**

```c
void test_ascii_subject_plain(void);   /* contains "\r\nSubject: Einladung\r\n" */
void test_umlaut_subject_encoded(void) {
  /* subject "Passwort zurücksetzen" -> contains
     "Subject: =?UTF-8?B?UGFzc3dvcnQgenVyw7xja3NldHplbg==?=" */
}
void test_header_injection_rejected(void);  /* to = "a@x.de\r\nBcc: b@y.de" -> -1 */
void test_body_lf_converted_to_crlf(void);
void test_buffer_too_small(void);           /* n = 10 -> -1 */
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** `feat(mail): rfc 5322 builder and libcurl smtp sender`

---

### Task 6: Booking rules

**Files:**
- Create: `server/src/core/booking_rules.{h,c}`
- Test: `server/test/test_booking_rules.c`

**Interfaces:**
- Consumes: `db_t`, `db_car`, `db_user` (Task 3).
- Produces:

```c
typedef enum { BR_OK = 0, BR_INVALID, BR_IN_PAST, BR_OVERLAP, BR_CAR_INACTIVE,
               BR_NOT_EXTENSION, BR_NOT_MODIFIABLE, BR_FORBIDDEN } br_result_t;
bool    interval_overlaps(int64_t a0, int64_t a1, int64_t b0, int64_t b1);   /* half-open */
int64_t booking_occupied_end(const booking_t *b, int64_t now);  /* ACTIVE: max(booked_end, now); else booked_end */
br_result_t br_check_free(const db_t *db, int64_t car_id, int64_t start, int64_t end,
                          int64_t exclude_id, int64_t now);   /* only RESERVED/ACTIVE block */
br_result_t br_check_create(const db_t *db, const user_t *actor, const booking_t *nb, int64_t now);
br_result_t br_check_update(const db_t *db, const user_t *actor, const booking_t *old,
                            const booking_t *nb, int64_t now);
br_result_t br_check_cancel(const user_t *actor, const booking_t *b, int64_t now);
int64_t br_cancel_fee(const settings_t *s, const booking_t *b, int64_t now);
```

Rules (decided here, from spec §5):
- `BR_INVALID`: `start >= end`, or a time not divisible by 60, or the car does not exist.
- `BR_CAR_INACTIVE`: car `active == false`.
- Create by user: `user_id` must equal `actor->id` (else `BR_FORBIDDEN`); `booked_start >= now - start_tolerance_min*60` (else `BR_IN_PAST`).
- Create by admin: any `user_id` of an active user; no time restriction.
- Update by user (own booking only, else `BR_FORBIDDEN`):
  - `RESERVED`: `car_id`/`user_id` unchanged, `new_start <= old_start`, `new_end >= old_end`, at least one changed, else `BR_NOT_EXTENSION`; a changed start must satisfy the create rule for `BR_IN_PAST`.
  - `ACTIVE`: start unchanged, `new_end >= old_end`, else `BR_NOT_EXTENSION`.
  - `DONE`/`CANCELLED`: `BR_NOT_MODIFIABLE`.
- Update by admin: any field, any state except `CANCELLED` (`BR_NOT_MODIFIABLE`).
- Every create/update of a `RESERVED`/`ACTIVE` booking runs `br_check_free` excluding itself.
- Cancel: state must be `RESERVED`; a user may cancel only their own booking and only when `now < booked_start`.
- Fee: `cancel_fee_cents` if `booked_start - now < cancel_deadline_min*60`, else 0.

- [ ] **Step 1: Write failing tests**

Fixture: car 1 active, car 2 inactive, user 1 (`ROLE_USER`), user 2 (`ROLE_USER`), admin 3. `T = 1774742400` (2026-03-29 00:00 UTC), `H = 3600`, `now = T`.

```c
void test_touching_intervals_do_not_overlap(void) {
  /* existing [T+10H, T+12H). create [T+12H, T+13H) -> BR_OK; [T+8H, T+10H) -> BR_OK;
     [T+11H+60, T+13H) -> BR_OVERLAP */
}
void test_create_start_must_be_minute_aligned(void);  /* T+10H+30 -> BR_INVALID */
void test_create_tolerance_allows_15min_past(void);   /* start now-900 OK, now-960 BR_IN_PAST */
void test_create_inactive_car(void);                  /* BR_CAR_INACTIVE */
void test_user_cannot_book_for_other_user(void);      /* BR_FORBIDDEN */
void test_user_extend_end_ok(void);
void test_user_extend_start_earlier_ok(void);
void test_user_shorten_rejected(void);                /* end earlier -> BR_NOT_EXTENSION */
void test_user_move_rejected(void);                   /* both shifted +1H -> BR_NOT_EXTENSION */
void test_user_extend_into_other_booking_overlap(void);
void test_active_booking_start_change_rejected(void);
void test_active_late_booking_blocks_until_now(void) {
  /* ACTIVE booking [T, T+2H), now = T+3H: create [T+2H, T+4H) by user 2 -> BR_OVERLAP */
}
void test_cancelled_does_not_block(void);
void test_user_cancel_after_start_rejected(void);
void test_admin_may_move_and_shorten(void);
void test_cancel_fee_inside_deadline(void);   /* deadline 60, fee 500: start in 30 min -> 500; in 90 min -> 0 */
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** `feat(core): booking rules`

---

### Task 7: API core, sessions, auth endpoints

**Files:**
- Create: `server/src/app.h`, `server/src/auth/session.{h,c}`, `server/src/http/api.{h,c}`, `server/src/http/api_auth.c`
- Modify: `server/test/helpers.{h,c}`
- Test: `server/test/test_session.c`, `server/test/test_api_auth.c`

**Interfaces:**
- Consumes: Tasks 2–5.
- Produces:

```c
/* app.h */
typedef struct {
  char listen[64], data_dir[256], web_dir[256], base_url[128];
  char smtp_url[128], smtp_user[64], smtp_pass[128], mail_from[128];
} config_t;
typedef int (*mail_send_fn)(void *ctx, const char *to, const char *subject, const char *body);
typedef struct {
  db_t db; config_t cfg; ratelimit_t rl;
  mail_send_fn mail_send; void *mail_ctx;
} app_t;

/* session.h  (token = 64 hex chars of 32 random bytes; stored as BLAKE2b-256 hex) */
int  session_create(app_t *a, int64_t user_id, int64_t now, char token_out[65]);  /* expires now+30d */
user_t *session_user(app_t *a, const char *token, int64_t now);   /* NULL if unknown/expired/user inactive */
int  session_destroy(app_t *a, const char *token);
int  token_issue(app_t *a, int64_t user_id, token_kind_t kind, int64_t now, char token_out[65]);
int64_t token_consume(app_t *a, const char *token, int64_t now);  /* user_id, or -1; marks used */

/* api.h */
typedef struct {
  const char *method, *path, *query, *body, *content_type, *session_token, *client_ip;
  size_t body_len; int64_t now;
} api_req_t;
typedef struct { int status; char *body; char set_cookie[256]; } api_res_t;  /* body malloc'd */
void api_handle(app_t *a, const api_req_t *req, api_res_t *res);
void api_res_free(api_res_t *res);
/* helpers for api_*.c */
void api_json(api_res_t *res, int status, cJSON *body);           /* takes ownership */
void api_error(api_res_t *res, int status, const char *code, const char *msg_de);
bool api_path_id(const char *path, const char *prefix, int64_t *id); /* "/api/bookings/12" */
bool api_query_int(const char *query, const char *key, int64_t *out);
int  api_send_mail(app_t *a, const char *to, const char *subject, const char *body);

/* helpers.h (tests) */
app_t *th_app(void);                       /* tmp data dir, fake mailer */
void th_free(app_t *a);
const char *th_last_mail_to(void); const char *th_last_mail_body(void); int th_mail_count(void);
int64_t th_add_user(app_t *a, const char *email, const char *pw, role_t role);
char *th_login(app_t *a, const char *email, const char *pw);    /* returns token, static buf */
api_res_t th_req(app_t *a, const char *method, const char *path, const char *body,
                 const char *token, int64_t now);           /* content_type json, ip "1.2.3.4" */
```

Router: a static table `{method, path pattern, handler, min_role}` with `min_role` ∈ `{PUBLIC, USER, ADMIN}`. Patterns use `mg_match`-style `*`; since `ocs_core` does not link Mongoose, implement a tiny `*` matcher in `api.c`. Results: unknown path `404`; known path but wrong method `405`; no session `401 {"error":"unauthenticated"}`; insufficient role `403`. `api_handle` also enforces `415` and `400` for invalid JSON on POST/PUT/DELETE with a body.

Endpoints in `api_auth.c`:

| Method/path | Body | Result |
|---|---|---|
| `POST /api/login` | `{email,password}` | 200 `{user}` + `set_cookie`; 401 `bad_credentials`; 429 `rate_limited` |
| `POST /api/logout` | – | 200, cookie cleared (`Max-Age=0`) |
| `POST /api/password/forgot` | `{email}` | always 200; mail only if user exists and is active |
| `POST /api/password/set` | `{token,password}` | 200; 400 `invalid_token`; 400 `password_too_short` |
| `GET /api/me` | – | 200 `{user}` (public json) |
| `PUT /api/me` | `{name,street,zip,city}` | 200 `{user}` |
| `PUT /api/me/password` | `{current,new}` | 200, other sessions of user destroyed; 400 `bad_credentials` |

Mail texts (German, exact subjects): `"Passwort zurücksetzen"` and `"Einladung zu open-carshare"`. The body contains the link `<base_url>/#/passwort?token=<token>`. `password/set` destroys all sessions of the user.

- [ ] **Step 1: Write failing tests**

```c
/* test_session.c */
void test_session_roundtrip(void);
void test_session_expired_after_30_days(void);    /* now+2592000 -> NULL */
void test_session_inactive_user_rejected(void);   /* set active=false -> NULL */
void test_session_stored_hashed(void);            /* raw token not in sessions.jsonl */
void test_token_single_use(void);                 /* second consume -> -1 */
void test_reset_token_expires_after_1h(void);     /* now+3601 -> -1 */
void test_invite_token_valid_7_days(void);        /* now+604799 -> ok */

/* test_api_auth.c */
void test_login_ok_sets_cookie(void) {
  /* status 200; set_cookie starts "ocs_session=" and contains
     "HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=2592000" */
}
void test_login_email_case_insensitive(void);
void test_login_wrong_pw_401(void);
void test_login_rate_limited_after_5_fails(void);   /* 6th -> 429 even with right pw */
void test_unknown_path_404(void);
void test_me_requires_session_401(void);
void test_non_json_content_type_415(void);
void test_malformed_json_400(void);                 /* body "{\"email\":" -> 400 */
void test_wrong_field_type_400(void);               /* {"email":5,"password":"x"} */
void test_forgot_unknown_email_200_no_mail(void);   /* th_mail_count()==0 */
void test_forgot_then_set_password_then_login(void);
void test_change_password_kills_other_sessions(void);
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement** `session.c`, then `api.c`, then `api_auth.c`, then `helpers.c`
- [ ] **Step 4: Run all tests, expect PASS** (`ctest --test-dir build --output-on-failure`)
- [ ] **Step 5: Commit** `feat(api): request core, sessions and auth endpoints`

---

### Task 8: User API: cars, calendar, bookings

**Files:**
- Create: `server/src/http/api_booking.c`
- Modify: `server/src/http/api.c` (routes)
- Test: `server/test/test_api_booking.c`

**Interfaces:**
- Consumes: booking rules (Task 6), API helpers (Task 7).
- Produces endpoints (all require `USER`):

| Method/path | Result |
|---|---|
| `GET /api/cars` | `[{id,name,plate,location}]`, active cars only |
| `GET /api/calendar?car=&from=&to=` | `[{id?,start,end,own,state?}]` of RESERVED/ACTIVE bookings overlapping `[max(from,now), to)`; for others only `{start,end,own:false}`, with `end` = `booking_occupied_end`. `to - from > 62*86400` → 400 `range_too_large` |
| `GET /api/bookings` | own RESERVED/ACTIVE with `booked_end > now` or ACTIVE, sorted by start, full booking json |
| `POST /api/bookings` | `{car_id,start,end}` → 201 booking |
| `PUT /api/bookings/{id}` | `{start,end}` → 200 booking |
| `DELETE /api/bookings/{id}` | 200 booking (state cancelled, `cancel_fee_cents` set) |
| `GET /api/settings/public` | `{start_tolerance_min,cancel_deadline_min,cancel_fee_cents}` |

`br_result_t` → HTTP: `BR_INVALID` 400 `invalid`, `BR_IN_PAST` 400 `in_past`, `BR_CAR_INACTIVE` 400 `car_inactive`, `BR_OVERLAP` 409 `overlap`, `BR_NOT_EXTENSION` 400 `only_extend`, `BR_NOT_MODIFIABLE` 400 `not_modifiable`, `BR_FORBIDDEN` 404 `not_found` (another user's booking id is not revealed). A failed `db_put_booking` → 500 `storage`.

German messages: `overlap` → "Das Auto ist in diesem Zeitraum bereits gebucht.", `only_extend` → "Buchungen können nur verlängert werden. Zum Verkürzen bitte stornieren und neu buchen.", `in_past` → "Der Beginn liegt in der Vergangenheit."

- [ ] **Step 1: Write failing tests**

```c
void test_cars_lists_only_active(void);
void test_calendar_hides_other_users_details(void) {
  /* user2's booking appears as object with exactly keys start,end,own(false) */
}
void test_calendar_hides_past_bookings(void);       /* booking ended before now not listed */
void test_calendar_range_limit_400(void);
void test_create_booking_201_and_persisted(void);   /* reload db from dir: booking present */
void test_create_overlap_409(void);
void test_extend_ok_shorten_400(void);
void test_cancel_sets_state_and_fee(void);
void test_other_users_booking_404(void);            /* PUT and DELETE */
void test_path_id_not_numeric_400(void);            /* PUT /api/bookings/abc */
void test_storage_failure_500_no_phantom(void);     /* chmod dir 0500; POST -> 500; calendar empty */
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** `feat(api): cars, calendar and booking endpoints`

---

### Task 9: Admin API

**Files:**
- Create: `server/src/http/api_admin.c`
- Modify: `server/src/http/api.c` (routes)
- Test: `server/test/test_api_admin.c`

**Interfaces:**
- Consumes: Tasks 6–8.
- Produces endpoints (all `ADMIN`):

| Method/path | Body / query | Result |
|---|---|---|
| `GET /api/admin/users` | – | all users (public json) |
| `POST /api/admin/users` | `{email,name,street,zip,city,role,license_checked_at?}` | 201; invite token issued + mail sent; `pw_hash` empty (login impossible until set); 409 `duplicate_email` |
| `PUT /api/admin/users/{id}` | same fields + `active` | 200; deactivating destroys sessions; an admin cannot deactivate or demote themselves (400 `self`) |
| `POST /api/admin/users/{id}/invite` | – | 200, new invite mail |
| `GET /api/admin/cars` | – | all cars |
| `POST /api/admin/cars` | `{name,plate,location,active}` | 201 |
| `PUT /api/admin/cars/{id}` | same | 200 |
| `GET /api/admin/bookings?from=&to=&car=` | – | all states, with user name and email |
| `POST /api/admin/bookings` | `{car_id,user_id,start,end}` | 201 |
| `PUT /api/admin/bookings/{id}` | `{car_id,user_id,start,end}` | 200 |
| `DELETE /api/admin/bookings/{id}` | – | 200 cancelled, fee 0 |
| `GET/PUT /api/admin/settings` | `{start_tolerance_min,cancel_deadline_min,cancel_fee_cents}` | 200; negative → 400 |

Emails are stored lowercased and trimmed; they must contain exactly one `@` (else 400 `invalid_email`).

- [ ] **Step 1: Write failing tests**

```c
void test_user_role_gets_403_on_admin(void);
void test_admin_create_user_sends_invite(void);  /* th_last_mail_to == email; body has "/#/passwort?token=" */
void test_admin_create_duplicate_email_409(void); /* "Anna@x.de" then "anna@x.de" */
void test_invited_user_cannot_login_before_set(void);
void test_deactivate_user_kills_session(void);   /* user's token -> /api/me 401 */
void test_admin_cannot_deactivate_self(void);
void test_admin_shorten_booking_ok(void);
void test_admin_booking_overlap_409(void);
void test_settings_roundtrip_and_negative_400(void);
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run, expect PASS**
- [ ] **Step 5: Commit** `feat(api): admin endpoints`

---

### Task 10: Config, Mongoose adapter, main, bootstrap admin

**Files:**
- Create: `server/src/config.{h,c}`, `server/src/http/server.c`, `server/src/main.c`
- Test: `server/test/test_config.c`

**Interfaces:**
- Consumes: `api_handle` (Task 7), `platform_*` (Tasks 4–5).
- Produces:

```c
int config_load(const char *path, config_t *cfg);   /* key = value, '#' comments; unknown key -> -1 */
int server_run(app_t *a);                           /* blocks; SIGTERM/SIGINT -> return 0 */
```

Config keys: `listen` (default `http://127.0.0.1:8080`), `data_dir`, `web_dir`, `base_url`, `smtp_url`, `smtp_user`, `smtp_pass`, `mail_from`. `data_dir`, `web_dir` and `base_url` are required.

CLI:
- `ocs-server -c <conf>` → run.
- `ocs-server -c <conf> create-admin <email> <name>` → reads the password from stdin (one line), creates an active admin, and exits 0. Exits 1 if the email exists or the password has fewer than 10 characters.

`server.c` behaviour:
- `/api/*` → `api_handle`, with `now = platform_now()`. Session token comes from the `ocs_session` cookie. `client_ip` is the first entry of `X-Forwarded-For` if the peer is `127.0.0.1`, else the peer address.
- Body > 65536 → 413.
- Everything else → `mg_http_serve_dir` on `web_dir`; `/` → `index.html`.
- Headers on all responses: `Content-Security-Policy: default-src 'self'; img-src 'self' data:`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: same-origin`. `/api` responses also get `Cache-Control: no-store`.
- `db_prune` once per hour via an `mg_timer`.
- Mail callback: `platform_mail_send` with a message from `mail_build`.

- [ ] **Step 1: Write failing tests** (`test_config.c`)

```c
void test_config_parses_values_and_comments(void);
void test_config_defaults_listen(void);
void test_config_unknown_key_fails(void);
void test_config_missing_required_fails(void);
```

- [ ] **Step 2: Run, expect FAIL**
- [ ] **Step 3: Implement** `config.c`, `server.c`, `main.c`
- [ ] **Step 4: Run tests, expect PASS**
- [ ] **Step 5: Manual end-to-end check**

```bash
mkdir -p /tmp/ocs-data && printf 'data_dir=/tmp/ocs-data\nweb_dir=web\nbase_url=http://localhost:8080\n' > /tmp/ocs.conf
echo 'adminpasswort1' | ./build/server/ocs-server -c /tmp/ocs.conf create-admin admin@x.de Admin
./build/server/ocs-server -c /tmp/ocs.conf &
curl -si -H 'Content-Type: application/json' -d '{"email":"admin@x.de","password":"adminpasswort1"}' localhost:8080/api/login | grep -E '^HTTP|Set-Cookie'
kill %1
```

Expected: `HTTP/1.1 200 OK` and a `Set-Cookie: ocs_session=` line. (Over plain HTTP the browser ignores `Secure` cookies; for local browser testing, put Caddy in front, as in Task 16.)

- [ ] **Step 6: Commit** `feat(server): config, mongoose adapter, main and create-admin`

---

### Task 11: Fuzz target for the API

**Files:**
- Create: `server/fuzz/fuzz_api.c`
- Modify: `server/CMakeLists.txt` (option `OCS_FUZZ`, clang only)

**Interfaces:**
- Consumes: `th_app`, `th_add_user`, `th_login`, `api_handle`.

The harness sets up an app once with one admin and one user session. Each input: byte 0 selects one of the POST/PUT/DELETE routes from the router table (modulo its count, with ids 1–3 in the path), byte 1 selects a session (none, user, admin), and the rest is the body. It asserts `res.status` is in {200, 201, 400, 401, 403, 404, 405, 409, 413, 415, 429}. The data dir is a tmpfs path under `/dev/shm`.

- [ ] **Step 1: Build**

Run: `CC=clang cmake -B build-fuzz -DOCS_FUZZ=ON && cmake --build build-fuzz --target fuzz_api`
Expected: binary `build-fuzz/server/fuzz_api`.

- [ ] **Step 2: Run 5 minutes**

Run: `./build-fuzz/server/fuzz_api -max_total_time=300 -max_len=4096`
Expected: ends with `Done ... runs` and no `ERROR: AddressSanitizer` or `deadly signal`. Fix any crash with a regression test in the matching `test_api_*.c` first.

- [ ] **Step 3: Commit** `test: libfuzzer target for api_handle`

---

### Task 12: Web foundation: Berlin time, API client, shell, login

**Files:**
- Create: `web/index.html`, `web/style.css`, `web/js/time.js`, `web/js/api.js`, `web/js/app.js`, `web/js/views/login.js`, `web/js/views/password.js`
- Test: `web/test/time.test.mjs`

**Interfaces:**
- Consumes: HTTP API (Tasks 7–9).
- Produces:

```js
// time.js — never uses the device time zone; all via Intl with timeZone 'Europe/Berlin'
export function berlinToEpoch(y, mo, d, h, mi)   // -> epoch seconds; DST gap: shift forward; DST overlap: first (CEST)
export function epochToBerlin(ts)                // -> {y, mo, d, h, mi, wd}  (wd: 1=Mo..7=So)
export function formatDateTime(ts)               // "Mo, 30.10.2026, 14:00"
export function formatTime(ts)                   // "14:00"
export function startOfBerlinDay(ts)             // epoch of 00:00 Berlin of that day
// api.js
export async function api(method, path, body)    // -> parsed JSON; throws ApiError{status, code, message}
// app.js — hash router: '#/login', '#/passwort', '#/kalender', '#/buchungen', '#/konto', '#/admin/*'
export function navigate(hash); export function currentUser();
```

Views export `render(root, params)`. On a 401, `app.js` redirects to `#/login`. Layout is mobile-first: top bar with the car name and a menu (Kalender, Meine Buchungen, Konto, Admin when role is admin, Abmelden). No external fonts, scripts or CDNs (CSP is `'self'`).

- [ ] **Step 1: Write failing tests**

```js
import { test } from 'node:test'; import assert from 'node:assert/strict';
import { berlinToEpoch, epochToBerlin, formatDateTime } from '../js/time.js';
test('winter time', () => assert.equal(berlinToEpoch(2026,3,29,1,0), 1774742400));
test('summer time', () => assert.equal(berlinToEpoch(2026,3,29,3,0), 1774746000));
test('dst gap shifts forward', () => assert.equal(berlinToEpoch(2026,3,29,2,30), 1774747800));
test('dst overlap takes first', () => assert.equal(berlinToEpoch(2026,10,25,2,30), 1792888200));
test('roundtrip', () => assert.deepEqual(
  (({y,mo,d,h,mi}) => ({y,mo,d,h,mi}))(epochToBerlin(1774746000)), {y:2026,mo:3,d:29,h:3,mi:0}));
test('format', () => assert.equal(formatDateTime(1774746000), 'So, 29.03.2026, 03:00'));
```

- [ ] **Step 2: Run, expect FAIL**

Run: `TZ=America/New_York node --test web/test/`
Expected: FAIL (module not found)

- [ ] **Step 3: Implement** `time.js`, then `api.js`, `app.js`, `index.html` (`<script type="module" src="js/app.js">`), `style.css`, `login.js` (E-Mail, Passwort, "Passwort vergessen?"), and `password.js` (with `token` in the hash query: set the new password twice; without a token: the forgot form).
- [ ] **Step 4: Run, expect PASS** (same command)
- [ ] **Step 5: Manual check**: the `Secure` cookie needs HTTPS, so run the server (Task 10 step 5) behind Caddy with its local CA: `caddy reverse-proxy --from localhost --to 127.0.0.1:8080`. Open `https://localhost` and log in as admin. Expected: the menu shows "Admin".
- [ ] **Step 6: Commit** `feat(web): berlin time, api client, shell and login`

---

### Task 13: Web calendar and bookings

**Files:**
- Create: `web/js/views/calendar.js`, `web/js/views/booking_form.js`, `web/js/views/my_bookings.js`
- Modify: `web/style.css`

**Interfaces:**
- Consumes: `time.js`, `api.js` (Task 12); `/api/cars`, `/api/calendar`, `/api/bookings` (Task 8).

Behaviour:
- **Calendar:** a car selector, shown only if more than 1 active car; otherwise the single car is preselected. Day view on width < 700 px, week view (Mo–So) otherwise, with "Heute", "‹" and "›". Rows are 30 min, 00:00–24:00 Berlin time. Own bookings blue, others grey labelled "belegt". Tapping a free slot opens the booking form with start at the slot and end at +2 h.
- **Booking form:** date `<input type="date">` plus a time `<select>` in 15-min steps, for start and end, converted with `berlinToEpoch`. Error messages come from the API `message`.
- **Meine Buchungen:** list of upcoming bookings. Each has "Verlängern" (form prefilled; the form prevents choosing an earlier end or later start, and the server enforces this too) and "Stornieren" (a confirmation dialog shows the fee from `/api/settings/public` if it is > 0, else "kostenlos").

- [ ] **Step 1: Implement** the three views.
- [ ] **Step 2: Run** `TZ=America/New_York node --test web/test/`. Expected: PASS (no regression).
- [ ] **Step 3: Manual check with Playwright MCP**, at 390×844 and 1280×800:
  1. As a user: create a booking for tomorrow 10:00–12:00. It appears blue.
  2. As a second user: the same slot shows grey "belegt" without a name; booking 11:00–13:00 shows the overlap message.
  3. Extend the first booking to 13:00. OK.
  4. Try to shorten it via the form. Not possible.
  5. Cancel it. It disappears.
- [ ] **Step 4: Commit** `feat(web): calendar, booking form and my bookings`

---

### Task 14: Web account and admin views

**Files:**
- Create: `web/js/views/account.js`, `web/js/views/admin_users.js`, `web/js/views/admin_cars.js`, `web/js/views/admin_bookings.js`, `web/js/views/admin_settings.js`

**Interfaces:**
- Consumes: `/api/me*` (Task 7), `/api/admin/*` (Task 9).

Behaviour:
- **Konto:** edit name and address; change password (current, new, repeat).
- **Admin Nutzer:** table (Name, E-Mail, Rolle, aktiv), create form, edit, "Einladung erneut senden".
- **Admin Autos:** list, create, edit, deactivate.
- **Admin Buchungen:** a filter by car and month, a list in all states, editing of start/end/car/user, and cancel.
- **Admin Einstellungen:** the three settings; the fee is entered in euros and converted to cents.

- [ ] **Step 1: Implement**
- [ ] **Step 2: Manual check with Playwright MCP:**
  1. Admin creates a user. The invite mail goes to the SMTP sink (`python3 -m aiosmtpd -n -l localhost:1025`, config `smtp_url=smtp://localhost:1025`).
  2. Open the link, set a password, log in.
  3. Admin shortens that user's booking. OK.
  4. Admin deactivates the user. The user's next click lands on the login page.
- [ ] **Step 3: Commit** `feat(web): account and admin views`

---

### Task 15: PWA

**Files:**
- Create: `web/manifest.json`, `web/sw.js`, `web/icon.svg`, `web/icon-192.png`, `web/icon-512.png`
- Modify: `web/index.html` (manifest link, `apple-touch-icon`, `theme-color`, service worker registration)

Decisions:
- Manifest: `name` "open-carshare", `short_name` "Carsharing", `lang` "de", `display` "standalone", `start_url` "/#/kalender".
- Service worker: on install, cache the app shell (`/`, `index.html`, `style.css`, all `js/**`, icons) under the cache name `ocs-v<N>`. `/api/*` is network only and never cached. Shell requests are cache-first, with a network update in the background. Bump `<N>` on every web change.
- Icons are rendered from `icon.svg`: `rsvg-convert -w 192 icon.svg > icon-192.png` (likewise 512). Install `librsvg2-bin` if missing.

- [ ] **Step 1: Implement**
- [ ] **Step 2: Lighthouse check:** run `lighthouse_audit` (chrome-devtools MCP) against `https://localhost` behind Caddy. Expected: the PWA installability check passes, and there are no console errors.
- [ ] **Step 3: Commit** `feat(web): pwa manifest, service worker, icons`

---

### Task 16: VPS deployment files

**Files:**
- Create: `deploy/vps/Caddyfile`, `deploy/vps/ocs-server.service`, `deploy/vps/ocs.conf.example`, `deploy/vps/README.md`

Decisions:
- `Caddyfile`: `{$OCS_DOMAIN} { reverse_proxy 127.0.0.1:8080 }`; for local testing, `localhost { reverse_proxy 127.0.0.1:8080 }`.
- systemd: user `ocs`, `ExecStart=/opt/ocs/ocs-server -c /etc/ocs/ocs.conf`, `Restart=on-failure`, `NoNewPrivileges=yes`, `ProtectSystem=strict`, `ReadWritePaths=/var/lib/ocs`, `PrivateTmp=yes`.
- Release build: `cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DOCS_SANITIZE=OFF`.
- README covers, in order: packages; build; create user `ocs` and the dirs (`/opt/ocs`, `/etc/ocs` mode 0750, `/var/lib/ocs`); config with SMTP relay; `create-admin`; systemd enable; Caddy; firewall (only 22, 80, 443); and an interim backup via a nightly `tar` of `/var/lib/ocs` in cron, until mail backups arrive in a later phase.

- [ ] **Step 1: Write the files**
- [ ] **Step 2: Verify the systemd unit:** `systemd-analyze verify deploy/vps/ocs-server.service`. Expected: no errors apart from the missing binary path.
- [ ] **Step 3: Verify the Caddyfile:** `caddy validate --config deploy/vps/Caddyfile --adapter caddyfile` (if caddy is installed). Expected: `Valid configuration`.
- [ ] **Step 4: Commit** `docs(deploy): vps caddy, systemd and setup guide`
