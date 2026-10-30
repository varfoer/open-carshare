// Shared date + 15-minute time select helpers (Berlin time).
import { berlinToEpoch, epochToBerlin } from './time.js';

export const p2 = (n) => String(n).padStart(2, '0');
export const dateStr = (b) => `${b.y}-${p2(b.mo)}-${p2(b.d)}`;
export const timeStr = (b) => `${p2(b.h)}:${p2(b.mi)}`;

export function field(label, ...controls) {
  const l = document.createElement('label');
  l.append(label, ...controls);
  return l;
}

// Date input + 15-minute time select. `extra` adds an off-grid time (existing bookings).
export function dateTime(ts) {
  const b = epochToBerlin(ts);
  const date = document.createElement('input');
  date.type = 'date';
  date.required = true;
  date.value = dateStr(b);
  const time = document.createElement('select');
  const opts = [];
  for (let m = 0; m < 1440; m += 15) opts.push(`${p2(m / 60 | 0)}:${p2(m % 60)}`);
  if (!opts.includes(timeStr(b))) { opts.push(timeStr(b)); opts.sort(); }
  for (const t of opts) {
    const o = document.createElement('option');
    o.value = t;
    o.textContent = t;
    time.append(o);
  }
  time.value = timeStr(b);
  return { date, time };
}

export function toEpoch({ date, time }) {
  const [y, mo, d] = date.value.split('-').map(Number);
  const [h, mi] = time.value.split(':').map(Number);
  return berlinToEpoch(y, mo, d, h, mi);
}
