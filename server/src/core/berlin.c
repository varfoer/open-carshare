#include "core/berlin.h"

static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

/* Howard Hinnant's civil-from-days / days-from-civil, proleptic Gregorian. */
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = floor_div(y, 400);
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t year_of_days(int64_t z) {
    z += 719468;
    int64_t era = floor_div(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    return yoe + era * 400 + (m <= 2);
}

/* Day number of the last Sunday of the 31-day month m (1970-01-01 = Thursday). */
static int64_t last_sunday(int64_t y, int m) {
    int64_t d = days_from_civil(y, m, 31);
    int64_t wd = ((d + 4) % 7 + 7) % 7; /* 0 = Sunday */
    return d - wd;
}

int64_t berlin_offset(int64_t utc) {
    int64_t y = year_of_days(floor_div(utc, 86400));
    int64_t start = last_sunday(y, 3) * 86400 + 3600;
    int64_t end = last_sunday(y, 10) * 86400 + 3600;
    return (utc >= start && utc < end) ? 7200 : 3600;
}

int64_t berlin_day_start(int64_t utc) {
    int64_t day = floor_div(utc + berlin_offset(utc), 86400) * 86400; /* local midnight as if UTC */
    /* Changes happen at 02:00/03:00 local, so local midnight is never ambiguous. */
    int64_t s = day - 7200;
    return berlin_offset(s) == 7200 ? s : day - 3600;
}

int64_t berlin_next_day_start(int64_t utc) {
    /* 25 h after local midnight lies in the next local day for 23, 24 and 25 h days. */
    return berlin_day_start(berlin_day_start(utc) + 25 * 3600);
}
