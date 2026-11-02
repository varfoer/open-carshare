/* libFuzzer target for the Codec 8 Extended parser. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "core/teltonika.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char imei[16];
  tlt_record_t recs[TLT_MAX_RECORDS];
  size_t used = 0, n = 0;
  if (tlt_parse_imei(data, size, imei, &used) == 1) assert(used <= size);
  used = 0;
  if (tlt_parse_packet(data, size, recs, &n, &used) == 1) {
    assert(used <= size);
    assert(n <= TLT_MAX_RECORDS);
  }
  return 0;
}
