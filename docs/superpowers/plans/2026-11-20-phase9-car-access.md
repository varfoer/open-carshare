# Phase 9: Car Access (igloohome key box) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Starting a trip on a car with an igloohome key box creates an hourly algoPIN through the igloohome API and shows it to the member until the return; admins pick and test boxes in the car form.

**Architecture:** Pure parts in `core/` (PIN window, igloohome request/response JSON, error texts); one HTTPS function in `platform/posix`; an igloohome client (`http/igloo_client.c`: token cache, 401 retry, 15 s budget, status mapping) with an injectable HTTP function; endpoints in a new `http/api_access.c`. `app_t` gets `pin_create`/`lock_list` function pointers (like `mail_send`), so API tests use a fake and never touch the network. PINs live in their own table `data/access_pins.jsonl`.

**Tech Stack:** unchanged: C11, CMake, Mongoose, cJSON, libcurl, Unity, vanilla JS (ES2020), `node --test`.

**Spec:** `docs/superpowers/specs/2026-11-20-car-access-design.md` (binding); `docs/spec.md` §4, §5, §7, §8, §11–§15 (updated in Task 9).

**Branch:** `phase9-car-access` (design doc already committed there).

**Out of scope:** igloohome Bridge, remote unlock/delete, one-time PINs, admin PINs for trips, other vendors, API credentials in the GUI, data migration (app not deployed).

## Builder calls (fixed here so tasks agree; change in review if wrong)

1. **Device ID format:** `[A-Za-z0-9._-]{1,63}`. Tighter than the design's "printable ASCII": the ID goes into a URL path, so `/`, `?`, `%`, `#` must be impossible.
2. **Files:** client in `server/src/http/igloo_client.{c,h}`, pure JSON in `server/src/core/igloo.{c,h}`, PIN window and error texts in `server/src/core/access.{c,h}`, endpoints in `server/src/http/api_access.c`.
3. **Variance attempts in memory:** `app_t.pin_attempts[16]` (`{car_id, hour, used}`); next variance = `max(db_pin_max_variance(car, hour), attempts.used) + 1`, recorded before the igloohome call. A full table replaces the entry with the smallest `hour`.
4. **Start response:** unchanged booking JSON; the web reads the PIN with `GET /pin` afterwards.
5. **`GET /pin` with no PIN rows:** `{"pin": null, "valid_from": 0, "valid_until": 0}`.
6. **Return dialog when the PIN cannot be fetched:** shows the `pin_unavailable` message and offers „Abbrechen“ and „Trotzdem zurückgeben“ (the member may already have put the key back with the old PIN).
7. **„Box prüfen“** is shown only for a saved car whose saved `lock_device_id` is non-empty, and is disabled with „Erst speichern“ while the select differs from the saved value.
8. **Device list parser (provisional):** igloohome does not document `GET /home/devices`. Accept a top-level array, or an object with an array under `payload`, `devices` or `data`; per item the ID from `deviceId` (fallback `id`), the name from `deviceName` (fallback `name`, else `""`); items without a valid ID are skipped. Rewrite against the first real answer during the hardware acceptance.
9. **Token request:** form body `grant_type=client_credentials`, no `scope` (igloohome grants all scopes of the client when omitted).

## Global Constraints

- All existing constraints hold: `int64_t` UTC seconds, no float, German UI / English code, fixed `char[]` reject-not-truncate, `-Wall -Wextra -Werror`, ASan/UBSan tests, db copy-before-modify and RAM restored on failed persist, `core/` without I/O.
- Build/test from repo root: `cmake --build build -j && ctest --test-dir build --output-on-failure`; web: `node --test web/test/`.
- Every changed web file stays listed in `web/sw.js` `SHELL`; `CACHE` bumped once per task that changes web files: `ocs-v22` → `ocs-v23` (Task 7), `ocs-v24` (Task 8), `ocs-v25` (Task 9).
- Test epoch `T = 1774742400` (2026-03-29 00:00 UTC, the CEST switch day), `H = 3600`; winter instant `W = 1795179600` (2026-11-20 13:00 UTC = 14:00 CET).
- Never write a PIN, token or client secret to stderr, mails or error bodies (the admin test PIN response excepted).
- Copy, verbatim:
  - `pin_unavailable` (503): „PIN für die Schlüsselbox konnte nicht abgerufen werden. Bitte erneut versuchen. Klappt es weiter nicht: Notfallkontakt auf der Hilfe-Seite.“
  - variance exhausted (503 `pin_unavailable`): „PIN für die Schlüsselbox konnte nicht abgerufen werden. Bitte ab <HH>:00 erneut versuchen. Klappt es weiter nicht: Notfallkontakt auf der Hilfe-Seite.“ (`<HH>` = local hour after `valid_from`)
  - `pin_too_soon` (429): „Eine neue PIN ist ab <HH:MM> möglich.“ (local time of newest `created_at + 1200`)
  - `duplicate_lock` (409): „Diese Schlüsselbox ist bereits einem Auto zugeordnet.“
  - `no_lock` (400): „Für dieses Auto ist keine Schlüsselbox eingetragen.“
  - return dialog: „Schlüssel in die Box legen und Box schließen.“
- Commit messages: Conventional Commits, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. A trip running past its PIN (overdue, or extended): `POST /pin {renew:false}` creates a PIN from the current hour to the hour after `max(booked_end, now + 1 h)`, never one that already ended. (Task 2 window tests, Task 5 `test_pin_post_overdue`)
2. igloohome answers 200 with a body that is not `{pin: 4..12 digits, pinId}`: treated as failure, booking stays `reserved`, nothing stored. (Task 2 parser tests, Task 5 `test_start_malformed_pin_keeps_reserved`)
3. Another member, or the owner of a `reserved`/`done` booking, asks for `/pin`: 404, no PIN in any body. (Task 5 `test_pin_get_foreign_404`, `test_pin_get_after_return_404`)
4. Server restarted within an hour that already used variances: the next attempt continues after the stored maximum instead of repeating variance 1. (Task 5 `test_variance_seeded_from_rows`)
5. The admin GDPR export and admin booking list after a trip with a PIN: no `pin` key, no digits. (Task 5 `test_export_has_no_pin`)

---

### Task 1: Hardware docs (FMC003, Simbase)

**Files:**
- Modify: `docs/spec.md` §8 (hardware paragraph), `deploy/vps/README.md` §10 (GPRS bullet), `docs/backlog.md` (`[P4] Tracker abuse`)

- [ ] **Step 1: Edit `docs/spec.md` §8 hardware paragraph**: "one Teltonika FMC003 per car in the OBD port (4G, LTE Cat 1; 2G-only models such as FMB003 are excluded, German 2G networks shut down in 2028), IoT SIM from Simbase with APN `simbase.eu` (internet breakout in Europe; the tracker sends GPS positions over plain TCP, so their transit stays in the EU)." Keep the sentence that the provider may change.
- [ ] **Step 2: Edit `deploy/vps/README.md` §10**: Simbase APN `simbase.eu` with one sentence why (EU breakout); keep the other providers' APNs.
- [ ] **Step 3: Edit `docs/backlog.md`**: retag `[P4] Tracker abuse` as `[any]` and drop "(prerequisite for Phase 4 billing)"; append "Simbase offers a static public IPv4 (plan and cost unknown), which would allow source-IP pinning."
- [ ] **Step 4: Commit**

```bash
git add docs/spec.md deploy/vps/README.md docs/backlog.md
git commit -m "docs: FMC003 and Simbase EU APN as bought hardware"
```

### Task 2: Pure core: PIN window, igloohome JSON, error texts

**Files:**
- Create: `server/src/core/access.h`, `server/src/core/access.c`, `server/src/core/igloo.h`, `server/src/core/igloo.c`
- Modify: `server/src/core/model.h` (add `access_pin_t`), `server/CMakeLists.txt` (`ocs_add_test(test_access)`, `ocs_add_test(test_igloo)`)
- Test: `server/test/test_access.c`, `server/test/test_igloo.c`

**Interfaces:**
- Produces (`core/model.h`):
  ```c
  typedef struct { int64_t id, booking_id, car_id; char pin_id[64]; char pin[16];
    int64_t valid_from, valid_until; int variance; int64_t created_at; } access_pin_t;
  /* booking_id 0 = admin test PIN; pin "" = hidden or test */
  ```
- Produces (`core/access.h`):
  ```c
  #define ACCESS_MAX_HOURS 672
  #define ACCESS_MARGIN_S 900
  #define ACCESS_RENEW_GAP_S 1200
  #define ACCESS_VARIANCE_MAX 3
  typedef enum { PIN_OK = 0, PIN_ERR_CONFIG, PIN_ERR_AUTH, PIN_ERR_PAYMENT, PIN_ERR_SCOPE,
    PIN_ERR_DEVICE, PIN_ERR_TIMEZONE, PIN_ERR_UNSUPPORTED, PIN_ERR_NETWORK, PIN_ERR_OTHER } pin_err_t;
  typedef struct { char pin[16]; char pin_id[64]; int http_status; /* last status on failure, 0 = transport */ } pin_result_t;
  typedef struct { char device_id[64]; char name[64]; } lock_info_t;
  void access_window(int64_t now, int64_t booked_end, int64_t *from, int64_t *until);
  bool access_usable(const access_pin_t *p, int64_t now);     /* pin[0] && now < valid_until - 900 */
  bool lock_device_id_valid(const char *s);                    /* builder call 1 */
  const char *pin_err_log(pin_err_t e);                        /* English, for stderr */
  const char *pin_err_admin_de(pin_err_t e);                   /* German, for admin responses */
  ```
- Produces (`core/igloo.h`):
  ```c
  /* "YYYY-MM-DDTHH:00:00+hh:00" in Europe/Berlin; -1 if utc % 3600 != 0 or cap too small. */
  int igloo_iso_hour(int64_t utc, char *out, size_t cap);
  /* malloc'd {"variance","startDate","endDate","accessName"}; NULL on OOM/invalid. */
  char *igloo_hourly_body(int64_t from, int64_t until, int variance, const char *access_name);
  int igloo_parse_pin(const char *body, pin_result_t *out);                        /* 0 ok, -1 malformed */
  int igloo_parse_token(const char *body, char *token, size_t cap, int64_t *expires_in);
  int igloo_parse_devices(const char *body, lock_info_t *out, size_t cap, size_t *n); /* builder call 8 */
  ```

- [ ] **Step 1: Write failing tests `test_access.c`**

```c
void test_window_examples(void) {
  int64_t f, u;
  access_window(T + 1200, T + 13500, &f, &u);   TEST_ASSERT_EQUAL_INT64(T, f); TEST_ASSERT_EQUAL_INT64(T + 4 * H, u);
  access_window(T + 3540, T + H, &f, &u);       TEST_ASSERT_EQUAL_INT64(T, f); TEST_ASSERT_EQUAL_INT64(T + 2 * H, u);
  access_window(T + 10 * H + 600, T + 5 * H, &f, &u); /* overdue */
  TEST_ASSERT_EQUAL_INT64(T + 10 * H, f); TEST_ASSERT_EQUAL_INT64(T + 12 * H, u);
  access_window(T + H, T + 5 * H, &f, &u);      TEST_ASSERT_EQUAL_INT64(T + 5 * H, u); /* end on full hour */
  access_window(T, T + 800 * H, &f, &u);        TEST_ASSERT_EQUAL_INT64(T + 672 * H, u); /* cap */
}
void test_usable_margin(void) {
  access_pin_t p = {.pin = "1234567", .valid_until = T + 4 * H};
  TEST_ASSERT_TRUE(access_usable(&p, T + 4 * H - 901));
  TEST_ASSERT_FALSE(access_usable(&p, T + 4 * H - 900));
  p.pin[0] = 0; TEST_ASSERT_FALSE(access_usable(&p, T));
}
void test_device_id_valid(void) {
  TEST_ASSERT_TRUE(lock_device_id_valid("SP2X1234ab"));
  TEST_ASSERT_TRUE(lock_device_id_valid("a.b_c-d"));
  TEST_ASSERT_FALSE(lock_device_id_valid(""));
  TEST_ASSERT_FALSE(lock_device_id_valid("a/b")); TEST_ASSERT_FALSE(lock_device_id_valid("a b"));
  TEST_ASSERT_FALSE(lock_device_id_valid("a?b")); /* + 64-char string -> false, 63 -> true */
}
void test_err_texts(void) {
  TEST_ASSERT_EQUAL_STRING("In der igloohome-App der Box eine Zeitzone (Europe/Berlin) zuweisen.",
                           pin_err_admin_de(PIN_ERR_TIMEZONE));
  TEST_ASSERT_EQUAL_STRING("igloo: device property has no timezone", pin_err_log(PIN_ERR_TIMEZONE));
}
```

- [ ] **Step 2: Write failing tests `test_igloo.c`**

```c
void test_iso_hour(void) {
  char b[32];
  TEST_ASSERT_EQUAL_INT(0, igloo_iso_hour(T, b, sizeof b));      TEST_ASSERT_EQUAL_STRING("2026-03-29T01:00:00+01:00", b);
  TEST_ASSERT_EQUAL_INT(0, igloo_iso_hour(T + H, b, sizeof b));  TEST_ASSERT_EQUAL_STRING("2026-03-29T03:00:00+02:00", b);
  TEST_ASSERT_EQUAL_INT(0, igloo_iso_hour(W, b, sizeof b));      TEST_ASSERT_EQUAL_STRING("2026-11-20T14:00:00+01:00", b);
  TEST_ASSERT_EQUAL_INT(-1, igloo_iso_hour(W + 60, b, sizeof b));
}
void test_hourly_body(void) {  /* parse result back with cJSON */
  char *s = igloo_hourly_body(W, W + 4 * H, 2, "Buchung 12");
  /* variance 2, startDate "2026-11-20T14:00:00+01:00", endDate "2026-11-20T18:00:00+01:00", accessName "Buchung 12" */
  /* variance 0 or 4 -> NULL */
}
void test_parse_pin(void) {
  pin_result_t r;
  TEST_ASSERT_EQUAL_INT(0, igloo_parse_pin("{\"pin\":\"5729314\",\"pinId\":\"AA9876B22D5E405C\"}", &r));
  TEST_ASSERT_EQUAL_STRING("5729314", r.pin); TEST_ASSERT_EQUAL_STRING("AA9876B22D5E405C", r.pin_id);
  TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("{\"pin\":\"57a9314\",\"pinId\":\"X\"}", &r));
  TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("{\"pin\":\"123\",\"pinId\":\"X\"}", &r));      /* < 4 digits */
  TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("{\"pin\":\"1234567890123\",\"pinId\":\"X\"}", &r)); /* > 12 */
  TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("{\"pin\":\"1234567\"}", &r));
  TEST_ASSERT_EQUAL_INT(-1, igloo_parse_pin("not json", &r));
}
void test_parse_token(void) {
  /* {"access_token":"abc","expires_in":86400,"token_type":"Bearer"} -> "abc", 86400; missing token or cap too small -> -1 */
}
void test_parse_devices(void) {
  /* [{"deviceId":"D1","deviceName":"Dörpsmobil 1"}] -> n 1;
     {"payload":[{"id":"D2","name":"B"},{"deviceId":"bad/id"}]} -> n 1 (D2, "B");
     {"x":1} -> -1; more items than cap -> first cap, n = cap */
}
```

- [ ] **Step 3: Run `cmake --build build -j && ctest --test-dir build -R "test_access|test_igloo" --output-on-failure`**; expected FAIL (undefined functions).
- [ ] **Step 4: Implement `core/access.c` and `core/igloo.c`.** Window per design: `H(t) = t - t % 3600`; `from = H(now)`; `until = H(max(booked_end, now + 3600) + 3599)`; `until = min(until, from + 672 h)`. ISO via `berlin_offset()` plus `gmtime_r` on `utc + offset`. `pin_err_admin_de` texts: CONFIG „Die igloohome-Zugangsdaten fehlen in ocs.conf.“, AUTH „igloohome lehnt die Zugangsdaten ab.“, PAYMENT „igloohome-API nicht bezahlt (Testzeitraum abgelaufen).“, SCOPE „Dem igloohome-Zugang fehlt eine Berechtigung (algopin-hourly, get-devices).“, DEVICE „Box bei igloohome nicht gefunden.“, TIMEZONE (above), UNSUPPORTED „Diese Box unterstützt keine PIN-Codes.“, NETWORK „igloohome nicht erreichbar.“, OTHER „Unerwartete Antwort von igloohome.“. `pin_err_log`: "igloo not configured", "igloo: credentials rejected", "igloo: payment required", "igloo: token scope lacks algopin-hourly", "igloo: unknown device", "igloo: device property has no timezone", "igloo: device does not support AlgoPIN", "igloo: unreachable", "igloo: unexpected answer".
- [ ] **Step 5: Run the tests again**; expected PASS.
- [ ] **Step 6: Commit** `feat(core): PIN window and igloohome JSON`.

### Task 3: Storage: car `lock_device_id`, `access_pins` table

**Files:**
- Modify: `server/src/core/model.h` (`car_t.lock_device_id[64]`), `server/src/core/codec.{h,c}`, `server/src/store/db.{h,c}`
- Test: `server/test/test_codec.c`, `server/test/test_db.c`

**Interfaces:**
- Consumes: `access_pin_t`, `lock_device_id_valid` (Task 2).
- Produces:
  ```c
  cJSON *access_pin_to_json(const access_pin_t *v);
  /* All keys required; pin "" or 4..12 digits; variance 1..3; booking_id >= 0. */
  int access_pin_from_json(const cJSON *j, access_pin_t *out);
  /* car: lock_device_id optional on load (absent -> ""), present must be "" or lock_device_id_valid. */
  car_t *db_car_by_lock(db_t *db, const char *device_id);      /* NULL for "" or no match */
  int db_put_pin(db_t *db, access_pin_t *p);                    /* id==0: max+1 insert; "access_pins.jsonl" */
  const access_pin_t *db_pin_newest(const db_t *db, int64_t booking_id); /* largest id; NULL none; booking_id > 0 */
  int db_pin_max_variance(const db_t *db, int64_t car_id, int64_t valid_from); /* 0 if none */
  int db_pins_blank(db_t *db, int64_t booking_id);  /* pin "" on all rows, one write; no write if nothing changes */
  int db_prune_pins(db_t *db, int64_t before);      /* drops created_at < before */
  ```

- [ ] **Step 1: Write failing tests**: `test_codec.c`: `test_access_pin_roundtrip`, `test_access_pin_rejects` (pin "12a4", variance 0 and 4, booking_id -1), `test_car_lock_optional` (absent → "", "a/b" → -1, roundtrip "SP2X"). `test_db.c`: `test_pins_put_newest_reload` (two rows of booking 7 → newest is the larger id; survives `db_free`/`db_load`), `test_pins_blank` (rows of booking 7 blanked, row of booking 8 untouched, persisted), `test_pins_max_variance` (car 1 hour T: rows v1, v2 → 2; other hour → 0), `test_prune_pins`, `test_car_by_lock`.
- [ ] **Step 2: Run** `ctest --test-dir build -R "test_codec|test_db" --output-on-failure`; expected FAIL.
- [ ] **Step 3: Implement**: codec functions next to the car/odometer codecs; `db_t` gets `access_pin_t *pins; size_t n_pins, cap_pins;`, loaded from `access_pins.jsonl` with `load_file` (missing file = empty), freed in `db_free`; `db_put_pin` via `DEFINE_PUT_ENTITY`; `db_pins_blank`/`db_prune_pins` follow `db_prune_readings` (copy, modify, `persist_table`, restore on failure).
- [ ] **Step 4: Run tests**; expected PASS.
- [ ] **Step 5: Commit** `feat(store): access_pins table and car lock_device_id`.

### Task 4: Config, HTTPS platform call, igloohome client

**Files:**
- Modify: `server/src/app.h` (config fields, function pointer types, `pin_attempts`), `server/src/config.{h,c}`, `server/src/platform/platform.h`, `server/src/platform/posix/platform_posix.c`, `server/src/main.c`, `deploy/vps/ocs.conf.example`, `server/CMakeLists.txt` (`ocs_add_test(test_igloo_client)`)
- Create: `server/src/http/igloo_client.{h,c}`
- Test: `server/test/test_config.c`, `server/test/test_igloo_client.c`

**Interfaces:**
- Consumes: Task 2 (`pin_err_t`, `pin_result_t`, `lock_info_t`, `igloo_*` JSON).
- Produces (`app.h`):
  ```c
  /* config_t additions */
  char igloo_client_id[128], igloo_client_secret[256], igloo_api_url[128], igloo_token_url[128];
  typedef pin_err_t (*pin_create_fn)(void *ctx, const char *device_id, int64_t from, int64_t until,
                                      int variance, const char *access_name, pin_result_t *out);
  typedef pin_err_t (*lock_list_fn)(void *ctx, lock_info_t *out, size_t cap, size_t *n, int *http_status);
  typedef struct { int64_t car_id, hour; int used; } pin_attempt_t;
  #define APP_PIN_ATTEMPTS 16
  /* app_t additions */ pin_create_fn pin_create; lock_list_fn lock_list; void *pin_ctx;
                        pin_attempt_t pin_attempts[APP_PIN_ATTEMPTS];
  ```
- Produces (`platform.h`):
  ```c
  typedef struct { const char *method, *url;          /* "GET" | "POST" */
    const char *basic_user, *basic_pass;              /* both NULL or both set */
    const char *bearer;                               /* NULL or token */
    const char *content_type, *body;                  /* NULL = no body */
    long timeout_s; } http_req_t;                     /* total; connect = min(5, timeout_s) */
  /* 0: answer received (*status set, out NUL-terminated); -1: transport error, timeout,
   * non-https URL or answer >= cap. TLS verification on. */
  int platform_https(const http_req_t *r, int *status, char *out, size_t cap);
  ```
- Produces (`http/igloo_client.h`):
  ```c
  typedef int (*igloo_http_fn)(const http_req_t *r, int *status, char *out, size_t cap);
  typedef struct { const config_t *cfg; igloo_http_fn http; int64_t (*now)(void);
    char token[4096]; int64_t token_exp; } igloo_client_t;   /* token_exp 0 = none */
  void igloo_client_init(igloo_client_t *c, const config_t *cfg, igloo_http_fn http, int64_t (*now)(void));
  pin_err_t igloo_pin_create(void *ctx, const char *device_id, int64_t from, int64_t until,
                             int variance, const char *access_name, pin_result_t *out);
  pin_err_t igloo_lock_list(void *ctx, lock_info_t *out, size_t cap, size_t *n, int *http_status);
  ```

- [ ] **Step 1: Write failing `test_config.c` cases**: `test_igloo_defaults` (no igloo keys → `igloo_api_url` "https://api.igloohome.co", `igloo_token_url` "https://auth.igloohome.co/oauth2/token", id/secret ""), `test_igloo_keys` (all four set → stored), `test_igloo_url_not_https` (`igloo_api_url = http://x` → -1), `test_igloo_url_trailing_slash` (→ -1).
- [ ] **Step 2: Write failing `test_igloo_client.c`** with a scripted fake `igloo_http_fn` (queue of `{status, body}` answers, records each request's method, URL, bearer, body, timeout_s) and a fake clock:
  - `test_pin_create_ok`: answers token 200 `{"access_token":"tok","expires_in":86400}`, PIN 200 `{"pin":"5729314","pinId":"AA98"}` → `PIN_OK`; second request URL `https://api.igloohome.co/home/devices/D1/algopin/hourly`, bearer "tok", body has `"variance":1`.
  - `test_token_cached`: two creates → one token request; clock + 86400 - 299 → new token request.
  - `test_401_retries_once`: token, PIN 401, token, PIN 200 → `PIN_OK`; PIN 401 twice → `PIN_ERR_AUTH`.
  - `test_status_mapping`: 402 → PAYMENT, 403 → SCOPE, 404 → DEVICE, 409 → TIMEZONE, 418 → UNSUPPORTED, 500 → OTHER (with `out->http_status` 500), token 400/401 → AUTH, transport -1 → NETWORK with `http_status` 0, PIN 200 malformed → OTHER.
  - `test_not_configured`: empty `igloo_client_id` → `PIN_ERR_CONFIG`, no request made.
  - `test_budget`: fake clock advances 9 s per request; token, PIN 401, token: third call gets `timeout_s` ≤ 15 - 18 → skipped, `PIN_ERR_NETWORK`; each request's `timeout_s` = 15 - elapsed.
  - `test_lock_list`: token, then `GET https://api.igloohome.co/home/devices` → parsed list; 402 → PAYMENT.
- [ ] **Step 3: Run** `ctest --test-dir build -R "test_config|test_igloo_client" --output-on-failure`; expected FAIL.
- [ ] **Step 4: Implement** config keys (https-only, no trailing `/`, defaults applied in `config_load`), `platform_https` (libcurl: `CURLOPT_PROTOCOLS_STR "https"`, `CURLOPT_CONNECTTIMEOUT min(5,timeout_s)`, `CURLOPT_TIMEOUT timeout_s`, `CURLOPT_NOSIGNAL`, write callback refusing more than `cap - 1` bytes, `CURLOPT_USERNAME/PASSWORD` with `CURLAUTH_BASIC` for basic, `Authorization: Bearer` header, `Accept: application/json`), and the client (token renewed when `now >= token_exp - 300`; one 401 retry; 15 s budget from the first request). The client never logs. Wire in `main.c`: a static `igloo_client_t`, `igloo_client_init(&ig, &a->cfg, platform_https, platform_now)`, `a->pin_create = igloo_pin_create`, `a->lock_list = igloo_lock_list`, `a->pin_ctx = &ig`. Add the four keys, commented, to `deploy/vps/ocs.conf.example`.
- [ ] **Step 5: Run tests**; expected PASS. Then the full suite (config struct grew).
- [ ] **Step 6: Commit** `feat(server): igloohome client and config`.

### Task 5: Start with PIN, PIN endpoints, hiding, pruning

**Files:**
- Create: `server/src/http/api_access.c`
- Modify: `server/src/http/api_internal.h`, `server/src/http/api.c` (routes), `server/src/http/api_booking.c` (`h_booking_start`, `h_booking_return`, `h_cars_get`), `server/src/http/api_admin.c` (`h_admin_booking_put`), `server/src/http/server.c` (`prune_cb`), `server/test/helpers.{h,c}` (fake PIN backend), `server/CMakeLists.txt` (`ocs_add_test(test_api_access)`)
- Test: `server/test/test_api_access.c`

**Interfaces:**
- Consumes: Tasks 2–4.
- Produces (`api_internal.h`):
  ```c
  /* Creates a PIN with igloohome and stores its row. 0 ok (*row filled); -1 igloohome failed (*err);
   * -2 variance exhausted for car and valid_from; -3 storage. booking_id 0 = admin test. */
  int api_access_create(app_t *a, int64_t car_id, const char *device_id, int64_t booking_id,
                        int64_t from, int64_t until, const char *access_name, int64_t now,
                        access_pin_t *row, pin_err_t *err);
  /* Writes the 503 pin_unavailable response for rc -1/-2 of api_access_create. */
  void api_pin_unavailable(api_res_t *res, int rc, int64_t valid_from);
  void h_booking_pin_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
  void h_booking_pin_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res);
  ```
  Routes: `GET /api/bookings/*/pin`, `POST /api/bookings/*/pin` (MIN_USER).
- Produces (`helpers.h`): `void th_pin_fail(pin_err_t e)` (PIN_OK = succeed), `void th_pin_malformed(int on)`, `int th_pin_count(void)`, `const char *th_pin_last_device(void)`, `int64_t th_pin_last_from(void)`, `int64_t th_pin_last_until(void)`, `int th_pin_last_variance(void)`, `const char *th_pin_last_name(void)`. The fake returns pin `"10000" + count` (e.g. "100001") and pinId `"P<count>"`; `th_app` resets it and wires `pin_create`/`lock_list`.

- [ ] **Step 1: Write failing tests `test_api_access.c`** (setup like `test_api_booking.c`; car1 with `lock_device_id "D1"`, car2 without):
  - `test_start_creates_pin`: booking car1 `T+H..T+5H`, start at `T+H+1200` → 200, state active; fake saw device "D1", from `T+H`, until `T+5H`, variance 1, name "Buchung <id>"; `GET /pin` → pin "100001", valid_until `T+5H`.
  - `test_start_without_lock_unchanged`: car2 start → 200, `th_pin_count() == 0`; `GET /pin` → 404.
  - `test_start_pin_failure_keeps_reserved`: `th_pin_fail(PIN_ERR_NETWORK)` → 503 `pin_unavailable` with the exact copy; booking still reserved; no `access_pin` row.
  - `test_start_malformed_pin_keeps_reserved`: `th_pin_malformed(1)` (fake returns `PIN_ERR_OTHER`) → 503, reserved.
  - `test_pin_get_foreign_404`: u2's token on u1's active booking → 404, body has no "pin".
  - `test_pin_get_after_return_404`: return → 200; `GET /pin` → 404; stored rows of the booking have `pin ""`.
  - `test_pin_post_usable_returns_same`: active, `POST /pin {}` before `valid_until - 900` → same pin, count unchanged.
  - `test_pin_post_overdue`: booked end `T+5H`, `POST /pin {"renew":false}` at `T+10H+600` → new PIN, from `T+10H`, until `T+12H`.
  - `test_pin_renew_too_soon`: start at `T+H+1200`, `POST /pin {"renew":true}` at `T+H+1800` → 429 `pin_too_soon` „Eine neue PIN ist ab 03:40 möglich.“ (`T+H+2400` = 01:40 UTC = 03:40 CEST); at `T+H+2400` → 200, variance 2.
  - `test_variance_counts_failures`: `th_pin_fail(PIN_ERR_NETWORK)`, start at `T+H` → 503; `th_pin_fail(PIN_OK)`, start at `T+H+60` → 200 with variance 2.
  - `test_variance_exhausted`: `th_pin_fail(PIN_ERR_NETWORK)`, starts at `T+H`, `T+H+60`, `T+H+120` → 503 each (variances 1..3 used); `th_pin_fail(PIN_OK)`, start at `T+H+180` → 503 „… Bitte ab 04:00 erneut versuchen. …“ (`T+2H` = 04:00 CEST), `th_pin_count()` unchanged by the fourth call; start at `T+2H` → 200, variance 1. (A single booking cannot exhaust an hour through renewals: the 1200 s throttle allows at most three PINs per hour.)
  - `test_variance_seeded_from_rows`: insert rows (car1, `valid_from T+H`, variance 2) directly with `db_put_pin`, fresh `pin_attempts`, start in that hour → variance 3.
  - `test_admin_edit_car_blanks`: admin PUT changes active booking's `car_id` → rows blanked; admin PUT changing only `end` → pin kept.
  - `test_export_has_no_pin`: after a trip with a PIN, `GET /api/admin/users/{u1}/export` and `GET /api/admin/bookings` bodies contain neither `"pin"` nor "100001".
  - `test_cars_has_lock`: `GET /api/cars` → car1 `has_lock: true`, car2 `false`, no `lock_device_id` key.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_api_access --output-on-failure`; expected FAIL.
- [ ] **Step 3: Implement.** `api_access_create`: next variance per builder call 3 (record the attempt before calling `a->pin_create`), > 3 → -2; on `PIN_OK` append row (`created_at = now`), else log `ocs-server: pin for booking <id> failed: <pin_err_log> (HTTP <status>)` and return -1. `h_booking_start`: after the existing checks, for a car with `lock_device_id` call it with `access_window(now, b.booked_end, …)` and access name `Buchung <id>`; -1/-2 → `api_pin_unavailable`, -3 → `api_storage_error`; only then set the booking active. `h_booking_pin_post`: owner, active, car has lock (else 404); body `{renew}` optional bool; `renew` false and newest usable → return it; `renew` true and newest `created_at + 1200 > now` → 429; else create with `access_window(now, booked_end)`. Response `{pin, valid_from, valid_until}`. `h_booking_return` and `h_admin_booking_put`: after a successful put, call `db_pins_blank` when the booking left `active` or changed `car_id`/`user_id` (a failure is logged, the response stays 200). `h_cars_get`: add `has_lock`. `prune_cb`: `db_prune_pins(&a->db, now - 90 * 86400)`.
- [ ] **Step 4: Run tests**; expected PASS. Full suite, then `cmake -B build-fuzz … && ./build-fuzz/server/fuzz_api -max_total_time=60` (new routes reachable).
- [ ] **Step 5: Commit** `feat(api): PIN on start and PIN endpoints`.

### Task 6: Admin box setup endpoints

**Files:**
- Modify: `server/src/http/api_admin.c` (`read_car_fields`), `server/src/http/api_access.c`, `server/src/http/api_internal.h`, `server/src/http/api.c`, `server/test/helpers.{h,c}`
- Test: `server/test/test_api_access.c`

**Interfaces:**
- Consumes: Tasks 2–5.
- Produces: `h_admin_locks_get` (`GET /api/admin/locks`), `h_admin_lock_test` (`POST /api/admin/cars/*/lock-test`), MIN_ADMIN. Helpers: `void th_locks_set(const lock_info_t *l, size_t n)`, `void th_locks_fail(pin_err_t e)`.

- [ ] **Step 1: Write failing tests**:
  - `test_car_lock_field`: admin POST car with `lock_device_id "D9"` → 201, stored; second car with "D9" → 409 `duplicate_lock` (exact copy); `"a/b"` → 400 `invalid`; PUT without the key keeps "D9"; `""` clears.
  - `test_admin_locks_get`: `th_locks_set({"D1","Dörpsmobil 1"},{"D2",""})` → 200 `[{"device_id":"D1","name":"Dörpsmobil 1"},{"device_id":"D2","name":""}]`; `th_locks_fail(PIN_ERR_PAYMENT)` → 503 `locks_unavailable` with message „igloohome-API nicht bezahlt (Testzeitraum abgelaufen).“; member token → 403.
  - `test_lock_test_ok`: car1 at `T+H+1200` → 200 `{"pin":"100001","valid_until":T+2H}`; fake saw from `T+H`, until `T+2H`, name "Test Auto <car id>"; stored row has `booking_id 0`, `pin ""`, variance 1.
  - `test_lock_test_errors`: car2 (no lock) → 400 `no_lock`; `th_pin_fail(PIN_ERR_TIMEZONE)` → 503 `lock_test_failed` with the TIMEZONE admin text; unknown car → 404.
- [ ] **Step 2: Run**; expected FAIL.
- [ ] **Step 3: Implement**: `read_car_fields` handles `lock_device_id` exactly like `tracker_imei` (absent keeps, `""` clears, `lock_device_id_valid`, `db_car_by_lock` duplicate check); `car_to_json` already carries it (Task 3), so admin car responses show it. `h_admin_locks_get` calls `a->lock_list` with a 32-entry buffer and logs failures as `ocs-server: igloo device list failed: <pin_err_log> (HTTP <status>)`. `h_admin_lock_test` uses `api_access_create` with `from = H(now)`, `until = from + 3600`, booking 0, then blanks nothing (row stored with `pin ""` and digits only in the response); -2 → 503 `lock_test_failed` „Für diese Stunde sind keine Test-PINs mehr möglich. Bitte ab <HH>:00 erneut versuchen.“
- [ ] **Step 4: Run tests**; expected PASS.
- [ ] **Step 5: Commit** `feat(api): admin box list and test PIN`.

### Task 7: Web: PIN on the booking card and in the return dialog

**Files:**
- Modify: `web/js/trip.js`, `web/js/views/my_bookings.js`, `web/style.css`, `web/sw.js` (`ocs-v23`)
- Create: `web/js/views/pin_dialog.js` (add to `SHELL`)
- Test: `web/test/trip.test.mjs`

**Interfaces:**
- Consumes: `GET/POST /api/bookings/{id}/pin`, `has_lock` in `/api/cars` (Task 5).
- Produces (`trip.js`): `export function pinView(p, nowSec)` → `{ kind: 'show', pin, until } | { kind: 'expired' } | { kind: 'none' }`; `'show'` iff `p && p.pin && nowSec < p.valid_until - 900`; `'expired'` iff `p && p.valid_until > 0` otherwise; else `'none'`.
- Produces (`pin_dialog.js`): `export function pinDialog(bookingId)` → `Promise<boolean>` (true = continue with the return).

- [ ] **Step 1: Write failing test** in `trip.test.mjs`:

```js
test('pinView', () => {
  assert.deepEqual(pinView({ pin: '5729314', valid_from: 0, valid_until: 10000 }, 9099), { kind: 'show', pin: '5729314', until: 10000 });
  assert.deepEqual(pinView({ pin: '5729314', valid_from: 0, valid_until: 10000 }, 9100), { kind: 'expired' });
  assert.deepEqual(pinView({ pin: null, valid_from: 0, valid_until: 10000 }, 0), { kind: 'expired' });
  assert.deepEqual(pinView({ pin: null, valid_from: 0, valid_until: 0 }, 0), { kind: 'none' });
});
```

- [ ] **Step 2: Run** `node --test web/test/`; expected FAIL.
- [ ] **Step 3: Implement**:
  - `my_bookings.js`: for an active booking whose car `has_lock`, fetch `GET /pin` and add a line: `'show'` → „PIN“ in large digits (class `pin-digits`), „Gültig bis <HH:MM>“ (`formatDateTime` if not today), link button „PIN funktioniert nicht?“ (`POST {renew:true}`, re-render; errors into the page's error line); `'expired'`/`'none'` → „PIN abgelaufen“ (only for `'expired'`) and button „Neue PIN holen“ (`POST {renew:false}`). Start errors (`pin_unavailable`) already surface through `carriedError`.
  - `pin_dialog.js`: fetches `GET /pin`; if not `'show'`, `POST {renew:false}`; shows the PIN, „Schlüssel in die Box legen und Box schließen.“, buttons „Abbrechen“ / „Weiter“; on fetch failure shows the server message and „Abbrechen“ / „Trotzdem zurückgeben“ (builder call 6).
  - `runTrip` for `return` on a car with a box: `await pinDialog(b.id)` before the damage dialog; false aborts.
  - `style.css`: `.pin-digits` (large, monospace, letter-spacing), both themes.
  - `sw.js`: `CACHE = 'ocs-v23'`, add `/js/views/pin_dialog.js`.
- [ ] **Step 4: Run** `node --test web/test/`; expected PASS. Manual check with `./build/server/ocs-server` and a fake: not possible without igloohome, so check rendering with a car without box (unchanged) and the 503 path (no `igloo_client_id` → start shows the copy).
- [ ] **Step 5: Commit** `feat(web): key box PIN on booking card and return`.

### Task 8: Web: box select and test in the admin car form

**Files:**
- Modify: `web/js/views/admin_cars.js`, `web/sw.js` (`ocs-v24`)

**Interfaces:**
- Consumes: `GET /api/admin/locks`, `POST /api/admin/cars/{id}/lock-test`, `lock_device_id` in admin car JSON (Tasks 3, 6).

- [ ] **Step 1: Implement** the field „Schlüsselbox“: on form open fetch `GET /api/admin/locks`; select with „Keine“ (`""`), one option per box (`name || device_id`), plus „Unbekannte Box (<id>)“ when the saved value is not in the list; on fetch failure a text input with the saved value and the server message as hint. Link „Anleitung“ to `help.html#schluesselbox-einrichten`. Button „Box prüfen“ per builder call 7; result line „Test-PIN <pin>, gültig bis <HH:MM> — an der Box eingeben. Öffnet sie, ist alles eingerichtet.“ or the error message. Send `lock_device_id` in the save body. List line: `· Box <name or id>` / `· ohne Box`.
- [ ] **Step 2: Verify** `node --test web/test/` still passes; manually in the browser with `igloo_client_id` unset: the form shows the text-input fallback with „Die igloohome-Zugangsdaten fehlen in ocs.conf.“, saving keeps the value.
- [ ] **Step 3: Commit** `feat(web): pick and test key box in car form`.

### Task 9: Spec, operator docs, help page

**Files:**
- Modify: `docs/spec.md` (§4, §5, §7, §11, §12, §13, §14, §15), `README.md`, `deploy/vps/README.md` (new section "igloohome key box"), `web/help.html`, `web/sw.js` (`ocs-v25`)

- [ ] **Step 1: Update `docs/spec.md`** per the design's "Spec changes": §4 car `lock_device_id`, keysafe removed, new `access_pin`; §5 start 503 `pin_unavailable`; §7 rewritten (hourly algoPIN at start, window, renew, hiding, no remote delete, cite the design doc); §11 outbound HTTPS to igloohome, config keys, secrets in `ocs.conf` only; §12 export excludes PINs; §13 the four new routes, `/keysafes` removed, `has_lock`; §14 drop "keysafe PIN algorithm"; §15 Phase 9 entry (done when the acceptance passes) and remove the keysafe line under "Later".
- [ ] **Step 2: `README.md`**: feature line „Car key in an igloohome key box, PIN shown at trip start“; drop the key safe from "Not yet done".
- [ ] **Step 3: `deploy/vps/README.md`** new section after §10: create the API client at `web.igloohome.co/api` (30-day trial, then about $2 per box and month), the four `ocs.conf` keys, restart; the box setup itself is the admin manual on the help page.
- [ ] **Step 4: `web/help.html`**: member section „Schlüsselbox“ (PIN after „Starten“, key back before „Zurückgeben“, „PIN funktioniert nicht?“, expired PIN, emergency contact) and admin section with `id="schluesselbox-einrichten"`: the three steps from the design (pair in the igloohome app with time zone Europe/Berlin and a name; choose in the car form; „Box prüfen“ and enter the test PIN). `sw.js` `CACHE = 'ocs-v25'`.
- [ ] **Step 5: Full verification**: `cmake --build build -j && ctest --test-dir build --output-on-failure` and `node --test web/test/` all pass.
- [ ] **Step 6: Commit** `docs: Phase 9 car access in spec, deploy guide and help`.

### Acceptance on hardware (product owner, after merge; not automatable)

From the design: device list parser against the real `GET /home/devices` answer (adjust builder call 8), box time zone Europe/Berlin, start → PIN opens, return within window → PIN opens, renewed PIN works, expired PIN does not, `igloo_client_id` unset → clean `pin_unavailable`, and whether used PINs fill the 199 slots (then a maintenance note in `deploy/vps/README.md`).
