// Booking card: date block, time range, badges, detail lines, actions. Shared by my bookings and history.
import { epochToBerlin, formatRange, sameBerlinDay, WD_DE, MONTH_DE_SHORT } from './time.js';
import { el } from './admin_ui.js';

export function dateBlock(ts, nowSec) {
  const b = epochToBerlin(ts);
  return el('div', { className: sameBerlinDay(ts, nowSec) ? 'date-block today' : 'date-block' },
    el('div', { className: 'wd', text: WD_DE[b.wd] }),
    el('div', { className: 'd', text: String(b.d) }),
    el('div', { className: 'mo', text: MONTH_DE_SHORT[b.mo] }));
}

// badges: [{ text, kind }] (kind: '', 'ok', 'accent', 'warn'); lines: strings or nodes; actions: nodes.
export function bookingCard({ start, end, nowSec, badges = [], lines = [], actions = [] }) {
  const top = el('div', { className: 'booking-top' }, el('p', { className: 'booking-time', text: formatRange(start, end) }));
  if (badges.length) {
    const wrap = el('span', {});
    for (const b of badges) wrap.append(' ', el('span', { className: b.kind ? `badge ${b.kind}` : 'badge', text: b.text }));
    top.append(wrap);
  }
  const main = el('div', { className: 'booking-main' }, top);
  for (const l of lines) main.append(typeof l === 'string' ? el('p', { className: 'booking-car', text: l }) : l);
  const card = el('article', { className: 'card booking' }, el('div', { className: 'booking-head' }, dateBlock(start, nowSec), main));
  if (actions.length) card.append(el('div', { className: 'booking-actions' }, ...actions));
  return card;
}
