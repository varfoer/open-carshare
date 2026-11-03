#include <stdio.h>
#include <string.h>

#include "auth/password.h"
#include "core/sepa.h"
#include "http/api_internal.h"

static bool creditor_ready(const settings_t *s) {
  return s->assoc_name[0] && s->assoc_street[0] && s->assoc_zip[0] && s->assoc_city[0] && s->creditor_id[0];
}

static bool add_str(cJSON *o, const char *k, const char *v) { return cJSON_AddStringToObject(o, k, v) != NULL; }

cJSON *mandate_api_json(const mandate_t *m) {
  static const char *const STATUS[] = {"active", "replaced", "revoked"};
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  char ref[24], masked[48];
  mandate_ref(m->id, ref, sizeof ref);
  iban_mask(m->iban, masked, sizeof masked);
  if (!cJSON_AddNumberToObject(o, "id", (double)m->id) || !add_str(o, "ref", ref) ||
      !add_str(o, "status", STATUS[m->status]) || !add_str(o, "holder", m->holder) ||
      !add_str(o, "iban_masked", masked) || !add_str(o, "bic", m->bic) ||
      !cJSON_AddNumberToObject(o, "created_at", (double)m->created_at) ||
      !cJSON_AddNumberToObject(o, "ended_at", (double)m->ended_at) ||
      !cJSON_AddNumberToObject(o, "ended_by", (double)m->ended_by) ||
      !add_str(o, "client_ip", m->client_ip) ||
      !cJSON_AddNumberToObject(o, "text_version", m->text_version) ||
      !add_str(o, "creditor_name", m->creditor_name) || !add_str(o, "creditor_id", m->creditor_id)) {
    cJSON_Delete(o);
    return NULL;
  }
  return o;
}

void h_me_mandate_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  const settings_t *s = &a->db.settings;
  bool ready = creditor_ready(s);
  cJSON *o = cJSON_CreateObject();
  const mandate_t *cur = db_mandate_current(&a->db, ctx->user->id);
  bool ok = o != NULL;
  if (ok) {
    cJSON *m = cur ? mandate_api_json(cur) : cJSON_CreateNull();
    if (m) cJSON_AddItemToObject(o, "mandate", m);
    ok = m != NULL;
  }
  ok = ok && cJSON_AddBoolToObject(o, "ready", ready) != NULL;
  if (ok) {
    char text[4096];
    if (ready && mandate_text(text, sizeof text, s) >= 0)
      ok = add_str(o, "text", text);
    else
      ok = !ready && cJSON_AddNullToObject(o, "text") != NULL;
  }
  ok = ok && (ready ? add_str(o, "creditor_id", s->creditor_id)
                    : cJSON_AddNullToObject(o, "creditor_id") != NULL);
  ok = ok && cJSON_AddNumberToObject(o, "text_version", MANDATE_TEXT_VERSION) != NULL;
  if (!ok) {
    cJSON_Delete(o);
    o = NULL;
  }
  api_json(res, 200, o);
}

static void respond(api_res_t *res, int status, const mandate_t *m, bool mail_sent, bool changed) {
  cJSON *o = cJSON_CreateObject();
  cJSON *mj = mandate_api_json(m);
  if (!o || !mj || !cJSON_AddBoolToObject(o, "mail_sent", mail_sent) ||
      !cJSON_AddBoolToObject(o, "changed", changed)) {
    cJSON_Delete(mj);
    cJSON_Delete(o);
    api_json(res, 500, NULL);
    return;
  }
  cJSON_AddItemToObjectCS(o, "mandate", mj);
  api_json(res, status, o);
}

static void trim(char *s) {
  size_t n = strlen(s), i = 0;
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
  while (s[i] == ' ' || s[i] == '\t') i++;
  if (i) memmove(s, s + i, strlen(s + i) + 1);
}

/* Ends every ACTIVE mandate of user_id except keep_id (0 = none kept) with status/actor.
 * Index loop with copy-then-put: pointers into the db die on a put.
 * Returns the number of rows ended, -1 on storage failure. */
static int end_active(app_t *a, int64_t user_id, int64_t keep_id, int64_t now,
                      mandate_status_t status, int64_t actor_id) {
  int n = 0;
  for (size_t i = 0; i < a->db.n_mandates; i++) {
    mandate_t m = a->db.mandates[i];
    if (m.user_id != user_id || m.status != MANDATE_ACTIVE || m.id == keep_id) continue;
    m.status = status;
    m.ended_at = now;
    m.ended_by = actor_id;
    if (db_put_mandate(&a->db, &m) != 0) return -1;
    n++;
  }
  return n;
}

void h_me_mandate_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const settings_t *s = &a->db.settings;
  if (!creditor_ready(s)) {
    api_error(res, 409, "no_creditor",
              "Lastschriftmandate sind noch nicht eingerichtet. Bitte wende dich an den Verein.");
    return;
  }
  mandate_t n;
  memset(&n, 0, sizeof n);
  const char *holder = api_get_str(ctx->body, "account_holder", sizeof n.holder);
  const char *iban = api_get_str(ctx->body, "iban", sizeof n.iban * 2);
  const char *cid = api_get_str(ctx->body, "creditor_id", sizeof n.creditor_id * 2);
  int64_t ver;
  bool accept;
  const cJSON *bic_item = ctx->body ? cJSON_GetObjectItemCaseSensitive(ctx->body, "bic") : NULL;
  const char *bic = bic_item ? api_get_str(ctx->body, "bic", sizeof n.bic * 2) : "";
  if (!holder || !iban || !bic || !cid || !api_get_int(ctx->body, "text_version", &ver) ||
      !api_get_bool(ctx->body, "accept", &accept) || !accept) {
    api_invalid(res);
    return;
  }
  snprintf(n.holder, sizeof n.holder, "%s", holder);
  trim(n.holder);
  size_t hl = api_utf8_len(n.holder);
  if (hl == 0 || hl > SEPA_HOLDER_MAX_CHARS) {
    api_invalid(res);
    return;
  }
  if (sepa_normalize(iban, n.iban, sizeof n.iban) != 0 || !iban_valid(n.iban)) {
    api_error(res, 400, "invalid_iban", "Die IBAN ist ungültig.");
    return;
  }
  if (sepa_normalize(bic, n.bic, sizeof n.bic) != 0 || (n.bic[0] && !bic_valid(n.bic))) {
    api_error(res, 400, "invalid_bic", "Die BIC ist ungültig.");
    return;
  }
  if (!n.bic[0] && iban_needs_bic(n.iban)) {
    api_error(res, 400, "bic_required", "Für diese IBAN ist die BIC erforderlich.");
    return;
  }
  char cid_n[sizeof n.creditor_id];
  if (sepa_normalize(cid, cid_n, sizeof cid_n) != 0 || strcmp(cid_n, s->creditor_id) != 0 ||
      ver != MANDATE_TEXT_VERSION) {
    api_error(res, 409, "text_changed", "Der Mandatstext wurde geändert. Bitte lies ihn erneut.");
    return;
  }
  const mandate_t *cur = db_mandate_current(&a->db, ctx->user->id);
  mandate_t old;
  bool have_old = cur != NULL;
  if (have_old) {
    old = *cur; /* copy: db pointers die on the next put */
    if (strcmp(old.holder, n.holder) == 0 && strcmp(old.iban, n.iban) == 0 &&
        strcmp(old.bic, n.bic) == 0) {
      if (end_active(a, old.user_id, old.id, req->now, MANDATE_REPLACED, old.user_id) < 0) {
        api_storage_error(res);
        return;
      }
      respond(res, 200, &old, false, false);
      return;
    }
  }
  n.user_id = ctx->user->id;
  n.status = MANDATE_ACTIVE;
  n.created_at = req->now;
  snprintf(n.client_ip, sizeof n.client_ip, "%s", req->client_ip ? req->client_ip : "");
  n.text_version = MANDATE_TEXT_VERSION;
  snprintf(n.creditor_name, sizeof n.creditor_name, "%s", s->assoc_name);
  snprintf(n.creditor_id, sizeof n.creditor_id, "%s", s->creditor_id);
  /* Insert first: a failure of the second write never leaves the user without a mandate. */
  if (db_put_mandate(&a->db, &n) != 0) {
    api_storage_error(res);
    return;
  }
  if (end_active(a, n.user_id, n.id, req->now, MANDATE_REPLACED, n.user_id) < 0) {
    api_storage_error(res);
    return;
  }
  char body[4096];
  bool sent = mandate_mail_body(body, sizeof body, ctx->user->name, &n, s) >= 0 &&
              api_send_mail(a, ctx->user->email, "SEPA-Lastschriftmandat erteilt", body) == 0;
  respond(res, 201, &n, sent, true);
}

int api_cancel_reserved(app_t *a, int64_t user_id, int64_t actor_id, int64_t now) {
  int n = 0;
  /* db_put_booking may invalidate pointers into the table: index, copy, put. */
  for (size_t i = 0; i < a->db.n_bookings; i++) {
    booking_t b = a->db.bookings[i];
    if (b.user_id != user_id || b.state != BK_RESERVED) continue;
    b.state = BK_CANCELLED;
    b.cancel_fee_cents = 0;
    b.cancelled_at = now;
    b.modified_at = now;
    b.modified_by = actor_id;
    if (db_put_booking(&a->db, &b) != 0) return -1;
    n++;
  }
  return n;
}

int api_mandate_revoke(app_t *a, int64_t user_id, int64_t actor_id, int64_t now) {
  int n = end_active(a, user_id, 0, now, MANDATE_REVOKED, actor_id);
  if (n < 0) return -1;
  if (n == 0) return -2;
  if (api_cancel_reserved(a, user_id, actor_id, now) < 0) return -1;
  const user_t *up = db_user(&a->db, user_id);
  if (!up) return -1;
  user_t u = *up;
  u.active = false;
  if (db_put_user(&a->db, &u) != 0) return -1;
  return db_delete_sessions(&a->db, NULL, user_id) == 0 ? 0 : -1;
}

static void revoke_result(api_res_t *res, int rc) {
  if (rc == 0) api_ok(res);
  else if (rc == -2) api_not_found(res);
  else api_storage_error(res);
}

void h_me_mandate_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  const char *pw = api_get_str(ctx->body, "password", API_PW_MAX_BYTES + 1);
  const user_t *me = ctx->user;
  if (!pw || !me->pw_hash[0] || !pw_verify(pw, me->pw_hash)) {
    api_error(res, 400, "bad_credentials", "Das Passwort ist falsch.");
    return;
  }
  int64_t uid = me->id;
  if (!db_mandate_current(&a->db, uid)) {
    api_not_found(res);
    return;
  }
  /* A revoked user could never return the car; the admin path stays open. */
  for (size_t i = 0; i < a->db.n_bookings; i++) {
    if (a->db.bookings[i].user_id == uid && a->db.bookings[i].state == BK_ACTIVE) {
      api_error(res, 409, "trip_active", "Bitte beende zuerst deine laufende Fahrt.");
      return;
    }
  }
  if (me->role == ROLE_ADMIN) {
    bool other = false;
    for (size_t i = 0; i < a->db.n_users; i++) {
      const user_t *u = &a->db.users[i];
      other = other || (u->role == ROLE_ADMIN && u->active && u->id != uid);
    }
    if (!other) {
      api_error(res, 409, "last_admin",
                "Du bist der letzte aktive Admin. Bitte ernenne zuerst einen weiteren Admin.");
      return;
    }
  }
  int rc = api_mandate_revoke(a, uid, uid, req->now);
  if (rc != 0) {
    revoke_result(res, rc);
    return;
  }
  api_clear_session_cookie(res);
  api_ok(res);
}

void h_admin_user_mandates_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  int64_t id;
  if (!api_segment_id(req->path, "/api/admin/users/", &id)) {
    api_invalid(res);
    return;
  }
  if (!db_user(&a->db, id)) {
    api_not_found(res);
    return;
  }
  cJSON *arr = cJSON_CreateArray();
  if (!arr) {
    api_storage_error(res);
    return;
  }
  for (size_t i = a->db.n_mandates; i-- > 0;) {
    const mandate_t *m = &a->db.mandates[i];
    if (m->user_id != id) continue;
    cJSON *o = mandate_api_json(m);
    if (!o) {
      cJSON_Delete(arr);
      api_json(res, 500, NULL);
      return;
    }
    cJSON_AddItemToArray(arr, o);
  }
  api_json(res, 200, arr);
}

void h_admin_user_mandate_revoke(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t id;
  if (!api_segment_id(req->path, "/api/admin/users/", &id)) {
    api_invalid(res);
    return;
  }
  if (id == ctx->user->id) {
    api_error(res, 400, "self", "Du kannst dich nicht selbst deaktivieren oder herabstufen.");
    return;
  }
  if (!db_user(&a->db, id)) {
    api_not_found(res);
    return;
  }
  revoke_result(res, api_mandate_revoke(a, id, ctx->user->id, req->now));
}
