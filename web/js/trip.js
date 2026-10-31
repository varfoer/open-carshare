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
