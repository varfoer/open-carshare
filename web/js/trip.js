// Pure helpers for the trip lifecycle buttons.
export function tripActions(b, nowSec) {
  const reserved = b.state === 'reserved';
  const active = b.state === 'active';
  return {
    start: reserved && nowSec < b.booked_end,
    ret: active,
    extend: reserved || active,
    cancel: reserved && nowSec < b.booked_start,
  };
}
