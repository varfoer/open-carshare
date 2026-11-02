#include <stdbool.h>
#include <string.h>
#include "core/pricing.h"
#include "core/berlin.h"

static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }
static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t trunc_min(int64_t t) { return t - t % 60; }

/* Minutes of [lo, hi) that lie in [ns, ne) (local minutes of day). */
static int64_t overlap(int64_t lo, int64_t hi, int64_t ns, int64_t ne) {
    int64_t a = max64(lo, ns), b = min64(hi, ne);
    return b > a ? b - a : 0;
}

/* Night minutes of the local-minute range [lo, hi). */
static int64_t night_of(const tariff_t *t, int64_t lo, int64_t hi) {
    if (t->night_start > t->night_end)
        return overlap(lo, hi, 0, t->night_end) + overlap(lo, hi, t->night_start, 1440);
    if (t->night_start < t->night_end)
        return overlap(lo, hi, t->night_start, t->night_end);
    return 0;
}

/* Add the minutes of the UTC interval [x, y) to *day / *night. The interval lies inside one
 * constant-offset segment with offset off; local midnight is l0 (local seconds). */
static void add_seg(const tariff_t *t, int64_t x, int64_t y, int64_t off, int64_t l0,
                    int64_t *day, int64_t *night) {
    if (y <= x) return;
    int64_t lo = (x + off - l0) / 60, hi = (y + off - l0) / 60;
    int64_t n = night_of(t, lo, hi);
    *night += n;
    *day += (hi - lo) - n;
}

/* Minutes of [x, y) within the local day [d0, d1), split by rate. A DST change inside the day
 * splits it into two constant-offset segments. */
static void add_day(const tariff_t *t, int64_t d0, int64_t d1, int64_t x, int64_t y,
                    int64_t *day, int64_t *night) {
    x = max64(x, d0); y = min64(y, d1);
    if (y <= x) return;
    int64_t off0 = berlin_offset(d0), l0 = d0 + off0;
    if (berlin_offset(d1 - 1) == off0) {
        add_seg(t, x, y, off0, l0, day, night);
        return;
    }
    int64_t lo = d0, hi = d1 - 1; /* first instant with another offset lies in (lo, hi] */
    while (hi - lo > 1) {
        int64_t mid = lo + (hi - lo) / 2;
        if (berlin_offset(mid) == off0) lo = mid; else hi = mid;
    }
    add_seg(t, x, min64(y, hi), off0, l0, day, night);
    add_seg(t, max64(x, hi), y, berlin_offset(hi), l0, day, night);
}

void price_compute(const tariff_t *t, int tolerance_min, int64_t b0, int64_t b1,
                   int64_t a0, int64_t a1, int64_t km, price_t *out) {
    memset(out, 0, sizeof *out);
    b0 = trunc_min(b0); b1 = trunc_min(b1);
    bool started = a0 != 0;
    a0 = trunc_min(a0); a1 = trunc_min(a1);
    if (started && a1 < a0) a1 = a0;
    if (!started) a0 = a1 = 0;

    int64_t s0 = started ? min64(b0, a0) : b0, s1 = started ? max64(b1, a1) : b1;
    /* unused = B minus A: up to two pieces */
    int64_t u0lo = b0, u0hi = b1, u1lo = b1, u1hi = b1;
    if (started) {
        u0hi = min64(b1, a0);
        u1lo = max64(b0, a1);
    }

    int64_t uf = t->unused_factor_permille;
    int64_t rd = t->rate_day_cents_per_h, rn = t->rate_night_cents_per_h;
    int64_t time_cents = 0, cut = 0;
    int days = 0;
    if (s1 > s0) {
        for (int64_t d0 = berlin_day_start(s0); d0 < s1; ) {
            int64_t d1 = berlin_next_day_start(d0);
            int64_t ad = 0, an = 0, ud = 0, un = 0;
            if (started) add_day(t, d0, d1, a0, a1, &ad, &an);
            add_day(t, d0, d1, u0lo, u0hi, &ud, &un);
            if (started) add_day(t, d0, d1, u1lo, u1hi, &ud, &un);
            out->day_min += ad; out->night_min += an;
            out->unused_day_min += ud; out->unused_night_min += un;
            int64_t cost = (rd * ad * 1000 + rn * an * 1000 + (rd * ud + rn * un) * uf + 30000) / 60000;
            if (t->time_cap_cents_per_day > 0 && cost > t->time_cap_cents_per_day) {
                cut += cost - t->time_cap_cents_per_day;
                cost = t->time_cap_cents_per_day;
            }
            time_cents += cost;
            days++;
            d0 = d1;
        }
    }
    out->days = days;
    out->time_cents = time_cents;
    out->time_cap_cut_cents = cut;

    if (started && a1 > b1 + (int64_t)tolerance_min * 60)
        out->late_min = (a1 - (b1 + (int64_t)tolerance_min * 60)) / 60;
    out->late_fee_cents = (out->late_min + 59) / 60 * t->late_fee_cents_per_started_h;

    if (km < 0) {
        out->status = PRICE_KM_PENDING;
        out->total_cents = time_cents + out->late_fee_cents;
        return;
    }
    out->status = PRICE_OK;
    out->km = km;
    out->km_cents = km * t->rate_km_cents;
    int64_t sum = time_cents + out->km_cents;
    if (t->combined_cap_cents_per_day > 0) {
        int64_t cap = (int64_t)days * t->combined_cap_cents_per_day;
        if (sum > cap) out->combined_cap_cut_cents = sum - cap;
    }
    out->total_cents = sum - out->combined_cap_cut_cents + out->late_fee_cents;
}

void price_booking(const tariff_t *t, const settings_t *s, const booking_t *b, price_t *out) {
    memset(out, 0, sizeof *out);
    if (b->state == BK_CANCELLED) {
        out->status = PRICE_OK;
        out->cancel_fee_cents = b->cancel_fee_cents;
        out->total_cents = b->cancel_fee_cents;
        return;
    }
    if (b->club_trip) { out->status = PRICE_OK; return; }
    if (!t) { out->status = PRICE_NO_TARIFF; return; }
    int64_t a0 = b->booked_start, a1 = b->booked_end, km = -1;
    if (b->state == BK_DONE) {
        a0 = b->actual_start;
        a1 = b->actual_end ? b->actual_end : max64(a0, b->booked_end);
        if (!b->km_pending) km = b->km_end > b->km_start ? b->km_end - b->km_start : 0;
    }
    price_compute(t, s->start_tolerance_min, b->booked_start, b->booked_end, a0, a1, km, out);
}
