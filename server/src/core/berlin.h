#ifndef OCS_CORE_BERLIN_H
#define OCS_CORE_BERLIN_H

#include <stdint.h>

/* Europe/Berlin from UTC seconds, pure integer arithmetic (no localtime/TZ).
 * EU rule: CEST from the last Sunday of March 01:00 UTC (inclusive) to the
 * last Sunday of October 01:00 UTC (exclusive). */

/* 3600 (CET) or 7200 (CEST). */
int64_t berlin_offset(int64_t utc);
/* UTC instant of local 00:00 of the local day containing utc. */
int64_t berlin_day_start(int64_t utc);
/* UTC instant of the following local 00:00 (23, 24 or 25 h later). */
int64_t berlin_next_day_start(int64_t utc);

#endif
