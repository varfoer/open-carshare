#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/billing.h"
#include "core/codec.h"
#include "core/pricing.h"
#include "core/sepa.h"
#include "http/api_internal.h"

/* Billing run API (admin). A draft is always computed live from the bookings; only a
 * FINAL run is read from the stored invoices and lines. */

typedef struct {
  int64_t user_id;
  size_t first, n;          /* range in calc_t.lines */
  int64_t total;
  const invoice_t *inv;     /* stored invoice (final runs), else NULL */
} group_t;

typedef struct {
  invoice_line_t *lines; size_t n_lines;
  group_t *groups; size_t n_groups;
  int64_t *km_pending, *no_tariff, *no_mandate;
  size_t n_km, n_nt, n_nm;
  int64_t total;
} calc_t;

static void calc_free(calc_t *c) {
  free(c->lines);
  free(c->groups);
  free(c->km_pending);
  free(c->no_tariff);
  free(c->no_mandate);
  memset(c, 0, sizeof *c);
}

typedef struct {
  const user_t *u;
  int64_t user_id;
  invoice_line_t line;
  bool km_pending, no_tariff;
} entry_t;

static int cmp_entry(const void *x, const void *y) {
  const entry_t *p = x, *q = y;
  int c = strcmp(p->u ? p->u->name : "", q->u ? q->u->name : "");
  if (c) return c;
  if (p->user_id != q->user_id) return p->user_id < q->user_id ? -1 : 1;
  if (p->line.start != q->line.start) return p->line.start < q->line.start ? -1 : 1;
  return p->line.booking_id < q->line.booking_id ? -1 : p->line.booking_id > q->line.booking_id;
}

static int cmp_line(const void *x, const void *y) {
  const invoice_line_t *p = x, *q = y;
  if (p->start != q->start) return p->start < q->start ? -1 : 1;
  return p->booking_id < q->booking_id ? -1 : p->booking_id > q->booking_id;
}

/* Live state at call time: expire reservations first, then price every included booking.
 * 0 ok, -1 storage or memory error. Users sorted by name (then id), lines by start (then booking id). */
static int calc_draft(app_t *a, const billing_run_t *run, int64_t now, calc_t *c) {
  memset(c, 0, sizeof *c);
  if (api_expire_bookings(a, now) < 0) return -1;
  const db_t *db = &a->db;
  int y, m;
  int64_t from, to;
  if (billing_period_parse(run->period, &y, &m) != 0) return -1;
  billing_period_bounds(y, m, &from, &to);
  size_t n = 0;
  for (size_t i = 0; i < db->n_bookings; i++)
    if (billing_includes(&db->bookings[i], to)) n++;
  entry_t *e = malloc((n + 1) * sizeof *e);
  c->lines = malloc((n + 1) * sizeof *c->lines);
  c->groups = malloc((n + 1) * sizeof *c->groups);
  c->km_pending = malloc((n + 1) * sizeof *c->km_pending);
  c->no_tariff = malloc((n + 1) * sizeof *c->no_tariff);
  c->no_mandate = malloc((n + 1) * sizeof *c->no_mandate);
  if (!e || !c->lines || !c->groups || !c->km_pending || !c->no_tariff || !c->no_mandate) {
    free(e);
    calc_free(c);
    return -1;
  }
  size_t k = 0;
  for (size_t i = 0; i < db->n_bookings; i++) {
    const booking_t *b = &db->bookings[i];
    if (!billing_includes(b, to)) continue;
    const car_t *car = db_car((db_t *)db, b->car_id);
    const tariff_t *t = db_tariff_at(db, b->car_id, b->booked_start);
    price_t p;
    price_booking(t, &db->settings, b, &p);
    entry_t *x = &e[k++];
    memset(x, 0, sizeof *x);
    x->u = db_user((db_t *)db, b->user_id);
    x->user_id = b->user_id;
    billing_line(b, car ? car->name : "", &p, &x->line);
    x->km_pending = b->km_pending;
    x->no_tariff = b->state != BK_CANCELLED && !b->club_trip && !t;
  }
  qsort(e, n, sizeof *e, cmp_entry);
  for (size_t i = 0; i < n; i++) {
    c->lines[i] = e[i].line;
    if (e[i].km_pending) c->km_pending[c->n_km++] = e[i].line.booking_id;
    if (e[i].no_tariff) c->no_tariff[c->n_nt++] = e[i].line.booking_id;
    if (c->n_groups == 0 || c->groups[c->n_groups - 1].user_id != e[i].user_id) {
      group_t *g = &c->groups[c->n_groups++];
      memset(g, 0, sizeof *g);
      g->user_id = e[i].user_id;
      g->first = i;
    }
    group_t *g = &c->groups[c->n_groups - 1];
    g->n++;
    g->total += e[i].line.total_cents;
    c->total += e[i].line.total_cents;
  }
  c->n_lines = n;
  free(e);
  for (size_t i = 0; i < c->n_groups; i++)
    if (c->groups[i].total > 0 && !db_mandate_current(db, c->groups[i].user_id))
      c->no_mandate[c->n_nm++] = c->groups[i].user_id;
  return 0;
}

/* Stored invoices and lines of a final run, ascending by invoice number. */
static int calc_final(const db_t *db, const billing_run_t *run, calc_t *c) {
  memset(c, 0, sizeof *c);
  size_t ni = 0;
  for (size_t i = 0; i < db->n_invoices; i++)
    if (db->invoices[i].run_id == run->id) ni++;
  c->lines = malloc((db->n_lines + 1) * sizeof *c->lines);
  c->groups = malloc((ni + 1) * sizeof *c->groups);
  const invoice_t **inv = malloc((ni + 1) * sizeof *inv);
  if (!c->lines || !c->groups || !inv) {
    free(inv);
    calc_free(c);
    return -1;
  }
  size_t k = 0;
  for (size_t i = 0; i < db->n_invoices; i++)
    if (db->invoices[i].run_id == run->id) inv[k++] = &db->invoices[i];
  /* insertion sort by number: runs are small and almost sorted */
  for (size_t i = 1; i < ni; i++)
    for (size_t j = i; j > 0 && inv[j - 1]->number > inv[j]->number; j--) {
      const invoice_t *t = inv[j];
      inv[j] = inv[j - 1];
      inv[j - 1] = t;
    }
  for (size_t i = 0; i < ni; i++) {
    group_t *g = &c->groups[c->n_groups++];
    g->user_id = inv[i]->user_id;
    g->inv = inv[i];
    g->total = inv[i]->total_cents;
    g->first = c->n_lines;
    for (size_t j = 0; j < db->n_lines; j++)
      if (db->lines[j].invoice_number == inv[i]->number) c->lines[c->n_lines++] = db->lines[j];
    g->n = c->n_lines - g->first;
    qsort(&c->lines[g->first], g->n, sizeof *c->lines, cmp_line);
    c->total += g->total;
  }
  free(inv);
  return 0;
}

/* ---- JSON ---- */

static bool jnum(cJSON *o, const char *k, int64_t v) { return cJSON_AddNumberToObject(o, k, (double)v) != NULL; }

static cJSON *id_array(const int64_t *v, size_t n) {
  cJSON *arr = cJSON_CreateArray();
  for (size_t i = 0; arr && i < n; i++) {
    cJSON *x = cJSON_CreateNumber((double)v[i]);
    if (!x || !cJSON_AddItemToArray(arr, x)) {
      cJSON_Delete(x);
      cJSON_Delete(arr);
      return NULL;
    }
  }
  return arr;
}

static cJSON *blockers_json(const calc_t *c) {
  cJSON *o = cJSON_CreateObject();
  cJSON *k = id_array(c->km_pending, c->n_km), *t = id_array(c->no_tariff, c->n_nt);
  cJSON *m = id_array(c->no_mandate, c->n_nm);
  if (!o || !k || !t || !m || !cJSON_AddItemToObject(o, "km_pending", k)) goto fail;
  k = NULL;
  if (!cJSON_AddItemToObject(o, "no_tariff", t)) goto fail;
  t = NULL;
  if (!cJSON_AddItemToObject(o, "no_mandate", m)) goto fail;
  return o;
fail:
  cJSON_Delete(o);
  cJSON_Delete(k);
  cJSON_Delete(t);
  cJSON_Delete(m);
  return NULL;
}

static cJSON *mandate_summary(const db_t *db, const group_t *g) {
  const mandate_t *m = NULL;
  if (g->inv && g->inv->mandate_id) m = db_mandate((db_t *)db, g->inv->mandate_id);
  if (!m) m = db_mandate_current(db, g->user_id);
  if (!m) return cJSON_CreateNull();
  char ref[24], masked[48];
  mandate_ref(m->id, ref, sizeof ref);
  iban_mask(m->iban, masked, sizeof masked);
  cJSON *o = cJSON_CreateObject();
  if (!o || !cJSON_AddStringToObject(o, "ref", ref) || !cJSON_AddStringToObject(o, "iban_masked", masked)) {
    cJSON_Delete(o);
    return NULL;
  }
  return o;
}

static cJSON *group_json(const db_t *db, const calc_t *c, const group_t *g) {
  const user_t *u = db_user((db_t *)db, g->user_id);
  cJSON *o = cJSON_CreateObject();
  cJSON *lines = cJSON_CreateArray();
  cJSON *mj = mandate_summary(db, g), *inv = NULL;
  if (!o || !lines || !mj) goto fail;
  if (g->inv) {
    inv = cJSON_CreateObject();
    if (!inv || !jnum(inv, "number", g->inv->number) || !jnum(inv, "mailed_at", g->inv->mailed_at)) goto fail;
  } else if (!(inv = cJSON_CreateNull())) {
    goto fail;
  }
  if (!jnum(o, "user_id", g->user_id) ||
      !cJSON_AddStringToObject(o, "name", g->inv ? g->inv->name : u ? u->name : "") ||
      !cJSON_AddStringToObject(o, "email", u ? u->email : "") || !jnum(o, "total_cents", g->total))
    goto fail;
  for (size_t i = 0; i < g->n; i++) {
    cJSON *l = invoice_line_to_json(&c->lines[g->first + i]);
    if (!l || !cJSON_AddItemToArray(lines, l)) {
      cJSON_Delete(l);
      goto fail;
    }
  }
  if (!cJSON_AddItemToObject(o, "mandate", mj)) goto fail;
  mj = NULL;
  if (!cJSON_AddItemToObject(o, "lines", lines)) goto fail;
  lines = NULL;
  if (!cJSON_AddItemToObject(o, "invoice", inv)) goto fail;
  return o;
fail:
  cJSON_Delete(o);
  cJSON_Delete(lines);
  cJSON_Delete(mj);
  cJSON_Delete(inv);
  return NULL;
}

/* Run detail; blockers only for a draft. NULL on OOM. */
static cJSON *detail_json(const db_t *db, const billing_run_t *run, const calc_t *c) {
  cJSON *o = cJSON_CreateObject();
  cJSON *r = billing_run_to_json(run), *users = cJSON_CreateArray();
  calc_t none;
  memset(&none, 0, sizeof none);
  cJSON *bl = blockers_json(run->state == RUN_DRAFT ? c : &none);
  if (!o || !r || !users || !bl) goto fail;
  if (!cJSON_AddItemToObject(o, "run", r)) goto fail;
  r = NULL;
  for (size_t i = 0; i < c->n_groups; i++) {
    cJSON *g = group_json(db, c, &c->groups[i]);
    if (!g || !cJSON_AddItemToArray(users, g)) {
      cJSON_Delete(g);
      goto fail;
    }
  }
  if (!cJSON_AddItemToObject(o, "users", users)) goto fail;
  users = NULL;
  if (!jnum(o, "total_cents", c->total)) goto fail;
  if (!cJSON_AddItemToObject(o, "blockers", bl)) goto fail;
  return o;
fail:
  cJSON_Delete(o);
  cJSON_Delete(r);
  cJSON_Delete(users);
  cJSON_Delete(bl);
  return NULL;
}

/* Detail for any run; computes live for a draft. res filled on error. */
static void respond_detail(app_t *a, const billing_run_t *run, int status, int64_t now, api_res_t *res) {
  calc_t c;
  int rc = run->state == RUN_DRAFT ? calc_draft(a, run, now, &c) : calc_final(&a->db, run, &c);
  if (rc != 0) { api_storage_error(res); return; }
  cJSON *j = detail_json(&a->db, run, &c);
  calc_free(&c);
  api_json(res, status, j);
}

/* ---- handlers ---- */

static const billing_run_t *run_from_path(app_t *a, const api_req_t *req, api_res_t *res) {
  int64_t id;
  if (!api_segment_id(req->path, "/api/admin/billing/", &id)) { api_invalid(res); return NULL; }
  const billing_run_t *r = db_run(&a->db, id);
  if (!r) api_not_found(res);
  return r;
}

void h_admin_billing_list(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)req;
  (void)ctx;
  const db_t *db = &a->db;
  cJSON *arr = cJSON_CreateArray();
  if (!arr) { api_storage_error(res); return; }
  /* newest first: ids ascend with creation */
  for (size_t i = db->n_runs; i-- > 0;) {
    const billing_run_t *r = &db->runs[i];
    int64_t n = 0, total = 0;
    for (size_t k = 0; k < db->n_invoices; k++)
      if (db->invoices[k].run_id == r->id) {
        n++;
        total += db->invoices[k].total_cents;
      }
    cJSON *o = billing_run_to_json(r);
    if (!o || !jnum(o, "n_invoices", n) || !jnum(o, "total_cents", total) || !cJSON_AddItemToArray(arr, o)) {
      cJSON_Delete(o);
      cJSON_Delete(arr);
      api_json(res, 500, NULL);
      return;
    }
  }
  api_json(res, 200, arr);
}

/* Value of key in "a=1&b=2" into out; false if absent or too long. */
static bool query_str(const char *q, const char *key, char *out, size_t cap) {
  size_t kl = strlen(key);
  for (const char *p = q; p && *p;) {
    if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
      size_t n = strcspn(p + kl + 1, "&");
      if (n >= cap) return false;
      memcpy(out, p + kl + 1, n);
      out[n] = 0;
      return true;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  return false;
}

void h_admin_billing_post(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  char ps[16];
  int y, m;
  if (!query_str(req->query, "period", ps, sizeof ps) || billing_period_parse(ps, &y, &m) != 0) {
    api_invalid(res);
    return;
  }
  int64_t from, to;
  billing_period_bounds(y, m, &from, &to);
  if (to > req->now) { api_error(res, 400, "period_open", "Der Monat ist noch nicht abgeschlossen."); return; }
  billing_run_t run;
  memset(&run, 0, sizeof run);
  snprintf(run.period, sizeof run.period, "%04d-%02d", y, m);
  for (size_t i = 0; i < a->db.n_runs; i++)
    if (a->db.runs[i].state == RUN_DRAFT) {
      api_error(res, 409, "draft_exists", "Es gibt bereits einen offenen Abrechnungsentwurf.");
      return;
    }
  for (size_t i = 0; i < a->db.n_runs; i++)
    if (strcmp(a->db.runs[i].period, run.period) >= 0) {
      api_error(res, 409, "period_exists", "Für diesen oder einen späteren Monat gibt es schon einen Abrechnungslauf.");
      return;
    }
  run.state = RUN_DRAFT;
  run.created_at = req->now;
  if (db_put_run(&a->db, &run) != 0) { api_storage_error(res); return; }
  api_json(res, 201, billing_run_to_json(&run));
}

void h_admin_billing_get(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  const billing_run_t *r = run_from_path(a, req, res);
  if (!r) return;
  respond_detail(a, r, 200, req->now, res);
}

static void err_not_draft(api_res_t *res) {
  api_error(res, 409, "not_draft", "Der Abrechnungslauf ist bereits abgeschlossen.");
}

void h_admin_billing_delete(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  const billing_run_t *r = run_from_path(a, req, res);
  if (!r) return;
  if (r->state != RUN_DRAFT) { err_not_draft(res); return; }
  if (db_delete_run(&a->db, r->id) != 0) { api_storage_error(res); return; }
  api_ok(res);
}

static void err_blocked(api_res_t *res, const calc_t *c) {
  cJSON *o = cJSON_CreateObject();
  cJSON *bl = blockers_json(c);
  if (!o || !bl || !cJSON_AddStringToObject(o, "error", "blocked") ||
      !cJSON_AddStringToObject(o, "message", "Abschluss nicht möglich.") ||
      !cJSON_AddItemToObject(o, "blockers", bl)) {
    cJSON_Delete(bl);
    cJSON_Delete(o);
    api_json(res, 500, NULL);
    return;
  }
  api_json(res, 409, o);
}

void h_admin_billing_finalize(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  (void)ctx;
  const billing_run_t *rp = run_from_path(a, req, res);
  if (!rp) return;
  if (rp->state != RUN_DRAFT) { err_not_draft(res); return; }
  const settings_t *s = &a->db.settings;
  if (!*s->assoc_name || !*s->assoc_street || !*s->assoc_zip || !*s->assoc_city || !*s->creditor_id ||
      !*s->creditor_iban) {
    api_error(res, 409, "no_creditor", "Bitte zuerst Vereins- und Gläubigerdaten eintragen.");
    return;
  }
  billing_run_t run = *rp;
  calc_t c;
  if (calc_draft(a, &run, req->now, &c) != 0) { api_storage_error(res); return; }
  if (c.n_km || c.n_nt || c.n_nm) {
    err_blocked(res, &c);
    calc_free(&c);
    return;
  }
  invoice_t *inv = calloc(c.n_groups + 1, sizeof *inv);
  if (!inv) { calc_free(&c); api_storage_error(res); return; }
  int64_t next = db_next_invoice_number(&a->db);
  for (size_t i = 0; i < c.n_groups; i++) {
    const group_t *g = &c.groups[i];
    const user_t *u = db_user(&a->db, g->user_id);
    invoice_t *v = &inv[i];
    v->number = next + (int64_t)i;
    v->user_id = g->user_id;
    v->run_id = run.id;
    v->total_cents = g->total;
    v->n_lines = (int64_t)g->n;
    v->created_at = req->now;
    snprintf(v->period, sizeof v->period, "%s", run.period);
    if (u) {
      snprintf(v->name, sizeof v->name, "%s", u->name);
      snprintf(v->street, sizeof v->street, "%s", u->street);
      snprintf(v->zip, sizeof v->zip, "%s", u->zip);
      snprintf(v->city, sizeof v->city, "%s", u->city);
    }
    for (size_t k = 0; k < g->n; k++) c.lines[g->first + k].invoice_number = v->number;
  }
  run.state = RUN_FINAL;
  run.finalized_at = req->now;
  int rc = db_finalize_run(&a->db, &run, inv, c.n_groups, c.lines, c.n_lines);
  free(inv);
  calc_free(&c);
  if (rc != 0) { api_storage_error(res); return; }
  const billing_run_t *fin = db_run(&a->db, run.id);
  if (!fin) { api_storage_error(res); return; }
  respond_detail(a, fin, 200, req->now, res);
}
