#include <stdio.h>
#include <string.h>

#include "core/sepa.h"
#include "http/api_internal.h"

static bool creditor_ready(const settings_t *s) { return s->assoc_name[0] && s->creditor_id[0]; }

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
    if (ready && mandate_text(text, sizeof text, s->assoc_name, s->creditor_id) >= 0)
      ok = add_str(o, "text", text);
    else
      ok = !ready && cJSON_AddNullToObject(o, "text") != NULL;
  }
  ok = ok && cJSON_AddNumberToObject(o, "text_version", MANDATE_TEXT_VERSION) != NULL;
  if (!ok) {
    cJSON_Delete(o);
    o = NULL;
  }
  api_json(res, 200, o);
}

static void respond(api_res_t *res, int status, const mandate_t *m, bool mail_sent) {
  cJSON *o = cJSON_CreateObject();
  cJSON *mj = mandate_api_json(m);
  if (!o || !mj || !cJSON_AddBoolToObject(o, "mail_sent", mail_sent)) {
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
  int64_t ver;
  bool accept;
  const cJSON *bic_item = ctx->body ? cJSON_GetObjectItemCaseSensitive(ctx->body, "bic") : NULL;
  const char *bic = bic_item ? api_get_str(ctx->body, "bic", sizeof n.bic * 2) : "";
  if (!holder || !iban || !bic || !api_get_int(ctx->body, "text_version", &ver) ||
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
  if (ver != MANDATE_TEXT_VERSION) {
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
      respond(res, 200, &old, false);
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
  if (have_old) {
    old.status = MANDATE_REPLACED;
    old.ended_at = req->now;
    old.ended_by = ctx->user->id;
    if (db_put_mandate(&a->db, &old) != 0) {
      api_storage_error(res);
      return;
    }
  }
  char body[4096];
  bool sent = mandate_mail_body(body, sizeof body, ctx->user->name, &n) >= 0 &&
              api_send_mail(a, ctx->user->email, "SEPA-Lastschriftmandat erteilt", body) == 0;
  respond(res, 201, &n, sent);
}
