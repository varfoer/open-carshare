// Berlin time helpers. Never uses the device time zone: everything goes
// through Intl with timeZone 'Europe/Berlin'.
const fmt = new Intl.DateTimeFormat('en-GB', {
  timeZone: 'Europe/Berlin', hourCycle: 'h23',
  year: 'numeric', month: 'numeric', day: 'numeric',
  hour: 'numeric', minute: 'numeric', weekday: 'short',
});
const WD = { Mon: 1, Tue: 2, Wed: 3, Thu: 4, Fri: 5, Sat: 6, Sun: 7 };
const WD_DE = ['', 'Mo', 'Di', 'Mi', 'Do', 'Fr', 'Sa', 'So'];

export function epochToBerlin(ts) {
  const p = {};
  for (const { type, value } of fmt.formatToParts(new Date(ts * 1000))) p[type] = value;
  return { y: +p.year, mo: +p.month, d: +p.day, h: +p.hour, mi: +p.minute, wd: WD[p.weekday] };
}

function offsetAt(ts) {
  const b = epochToBerlin(ts);
  return Date.UTC(b.y, b.mo - 1, b.d, b.h, b.mi) / 1000 - Math.floor(ts / 60) * 60;
}

export function berlinToEpoch(y, mo, d, h, mi) {
  const guess = Date.UTC(y, mo - 1, d, h, mi) / 1000;
  // Candidates: CEST (+2h) first, then CET (+1h); earliest match wins (overlap -> CEST).
  for (const off of [7200, 3600]) {
    const t = guess - off;
    const b = epochToBerlin(t);
    if (b.y === y && b.mo === mo && b.d === d && b.h === h && b.mi === mi) return t;
  }
  // DST gap: interpret with the offset in force before the gap -> shifts forward.
  return guess - offsetAt(guess - 86400);
}

export function formatTime(ts) {
  const b = epochToBerlin(ts);
  return `${String(b.h).padStart(2, '0')}:${String(b.mi).padStart(2, '0')}`;
}

export function formatDateTime(ts) {
  const b = epochToBerlin(ts);
  const p = (n) => String(n).padStart(2, '0');
  return `${WD_DE[b.wd]}, ${p(b.d)}.${p(b.mo)}.${b.y}, ${formatTime(ts)}`;
}

export function startOfBerlinDay(ts) {
  const b = epochToBerlin(ts);
  return berlinToEpoch(b.y, b.mo, b.d, 0, 0);
}
