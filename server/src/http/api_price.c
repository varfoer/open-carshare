#include <string.h>

#include "core/booking_rules.h"
#include "core/codec.h"
#include "core/pricing.h"
#include "http/api_internal.h"

#define ESTIMATE_KM_MAX 10000000
#define ESTIMATE_SPAN_MAX (90 * 86400)

/* Every price_t number by its field name; "tariff" is the version used or null. NULL on OOM. */
cJSON *price_to_json(const price_t *p, const tariff_version_t *t) {
  static const char *const status[] = {"ok", "km_pending", "no_tariff"};
  cJSON *o = cJSON_CreateObject();
  if (!o) return NULL;
  cJSON *tj = t ? tariff_version_to_json(t) : cJSON_CreateNull();
  if (!cJSON_AddStringToObject(o, "status", status[p->status]) ||
      !cJSON_AddNumberToObject(o, "day_min", (double)p->day_min) ||
      !cJSON_AddNumberToObject(o, "night_min", (double)p->night_min) ||
      !cJSON_AddNumberToObject(o, "unused_day_min", (double)p->unused_day_min) ||
      !cJSON_AddNumberToObject(o, "unused_night_min", (double)p->unused_night_min) ||
      !cJSON_AddNumberToObject(o, "days", p->days) ||
      !cJSON_AddNumberToObject(o, "time_cents", (double)p->time_cents) ||
      !cJSON_AddNumberToObject(o, "time_cap_cut_cents", (double)p->time_cap_cut_cents) ||
      !cJSON_AddNumberToObject(o, "km", (double)p->km) ||
      !cJSON_AddNumberToObject(o, "km_cents", (double)p->km_cents) ||
      !cJSON_AddNumberToObject(o, "combined_cap_cut_cents", (double)p->combined_cap_cut_cents) ||
      !cJSON_AddNumberToObject(o, "late_min", (double)p->late_min) ||
      !cJSON_AddNumberToObject(o, "late_fee_cents", (double)p->late_fee_cents) ||
      !cJSON_AddNumberToObject(o, "cancel_fee_cents", (double)p->cancel_fee_cents) ||
      !cJSON_AddNumberToObject(o, "total_cents", (double)p->total_cents) || !tj ||
      !cJSON_AddItemToObject(o, "tariff", tj)) {
    cJSON_Delete(tj);
    cJSON_Delete(o);
    return NULL;
  }
  return o;
}

/* booking_to_json plus "price" (tariff of the user at booked_start). NULL on OOM. */
cJSON *api_booking_json(app_t *a, const booking_t *b) {
  cJSON *o = booking_to_json(b);
  if (!o) return NULL;
  const tariff_version_t *t = db_tariff_at(&a->db, b->user_id, b->booked_start);
  price_t p;
  price_booking(t, &a->db.settings, b, &p);
  cJSON *pj = price_to_json(&p, t);
  if (!pj || !cJSON_AddItemToObject(o, "price", pj)) {
    cJSON_Delete(pj);
    cJSON_Delete(o);
    return NULL;
  }
  return o;
}

/* club= is optional; when present it must be 1. */
static bool query_club(const char *query, bool *club) {
  *club = false;
  for (const char *p = query; p && *p;) {
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= 5 && strncmp(p, "club=", 5) == 0) {
      if (len != 6 || p[5] != '1') return false;
      *club = true;
    }
    p = end ? end + 1 : NULL;
  }
  return true;
}

void h_price_estimate(app_t *a, const api_req_t *req, const api_ctx_t *ctx, api_res_t *res) {
  int64_t start, end, km;
  bool club;
  if (!api_query_int(req->query, "start", &start) || !api_query_int(req->query, "end", &end) ||
      !api_query_int(req->query, "km", &km) ||
      !query_club(req->query, &club) || end <= start || km < 0 || km > ESTIMATE_KM_MAX ||
      start <= 0 || !br_time_in_range(start, req->now) || !br_time_in_range(end, req->now) ||
      end - start > ESTIMATE_SPAN_MAX) {
    api_invalid(res);
    return;
  }
  const tariff_version_t *t = db_tariff_at(&a->db, ctx->user->id, start);
  price_t p;
  if (club) {
    memset(&p, 0, sizeof p);
    p.status = PRICE_OK;
  } else if (!t) {
    memset(&p, 0, sizeof p);
    p.status = PRICE_NO_TARIFF;
  } else {
    price_compute(t, a->db.settings.start_tolerance_min, start, end, start, end, km, &p);
  }
  api_json(res, 200, price_to_json(&p, t));
}
