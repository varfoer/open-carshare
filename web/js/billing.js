// Pure helpers for the billing admin view.
import { epochToBerlin } from './time.js';

const MONTHS = ['Januar', 'Februar', 'März', 'April', 'Mai', 'Juni', 'Juli', 'August',
  'September', 'Oktober', 'November', 'Dezember'];

// "YYYY-MM" of the Berlin month before the one `now` falls in.
export function lastMonth(now) {
  const b = epochToBerlin(Math.floor(now.getTime() / 1000));
  const y = b.mo === 1 ? b.y - 1 : b.y;
  const mo = b.mo === 1 ? 12 : b.mo - 1;
  return `${y}-${String(mo).padStart(2, '0')}`;
}

// "2026-10" -> "Oktober 2026"
export function periodLabel(period) {
  const m = /^(\d{4})-(\d{2})$/.exec(period);
  if (!m || +m[2] < 1 || +m[2] > 12) return period;
  return `${MONTHS[+m[2] - 1]} ${m[1]}`;
}

// "2026-11-09" -> "09.11.2026"
export function dateDe(iso) {
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(iso);
  return m ? `${m[3]}.${m[2]}.${m[1]}` : iso;
}

// German blocker lines; usersById is a Map or plain object of user id -> { name }.
export function blockerLines(blockers, usersById) {
  const nameOf = (id) => {
    const u = usersById instanceof Map ? usersById.get(id) : usersById && usersById[id];
    return u && u.name ? u.name : `Nutzer ${id}`;
  };
  return [
    ...(blockers.km_pending || []).map((id) => `Buchung ${id}: km fehlen`),
    ...(blockers.no_tariff || []).map((id) => `Buchung ${id}: kein Tarif`),
    ...(blockers.no_mandate || []).map((id) => `${nameOf(id)}: kein SEPA-Mandat`),
  ];
}
