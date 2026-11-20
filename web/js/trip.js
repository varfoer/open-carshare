// Pure helpers for the trip lifecycle buttons.
export function tripActions(b, nowSec) {
  const reserved = b.state === 'reserved';
  const active = b.state === 'active';
  return {
    start: reserved && nowSec < b.booked_end,
    ret: active,
    report: active,
    extend: reserved || active,
    cancel: reserved && nowSec < b.booked_start,
  };
}

// Server limit: damage text strlen < 1024 bytes (UTF-8).
export const DAMAGE_TEXT_MAX_BYTES = 1023;
export function damageTextTooLong(text) {
  return new TextEncoder().encode(text).length > DAMAGE_TEXT_MAX_BYTES;
}

// Key box PIN state from GET /api/bookings/{id}/pin. The server stops handing out a PIN
// 15 min before it ends, so the card does the same.
export function pinView(p, nowSec) {
  if (p && p.pin && nowSec < p.valid_until - 900) return { kind: 'show', pin: p.pin, until: p.valid_until };
  if (p && p.valid_until > 0) return { kind: 'expired' };
  return { kind: 'none' };
}
