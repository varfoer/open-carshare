#ifndef OCS_HTTP_TRACKER_H
#define OCS_HTTP_TRACKER_H

/* Teltonika tracker session: IMEI handshake, then Codec 8E packets that are
 * stored as odometer readings and acknowledged. Transport-free (no Mongoose). */

#include <stddef.h>
#include <stdint.h>

#include "app.h"

typedef struct { int64_t car_id; int64_t last_rx; } tracker_conn_t;  /* car_id 0: before handshake */

/* Handles at most one message from buf. *used = bytes consumed (0: need more).
 * reply (cap >= 4) gets the bytes to send, *reply_len may be 0.
 * Returns 0 keep open, -1 close after sending reply. */
int tracker_feed(app_t *a, tracker_conn_t *st, const uint8_t *buf, size_t len, int64_t now,
                 uint8_t *reply, size_t *reply_len, size_t *used);

#endif
