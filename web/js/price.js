// Price display for the server's booking `price` object. Money stays integer cents.
import { formatEuro } from './money.js';
import { formatTime, formatDateTime, sameBerlinDay } from './time.js';

// Permille -> German percent text: 500 -> "50", 125 -> "12,5".
const pct = (permille) => String(permille / 10).replace('.', ',');

export function priceSummary(price) {
  if (price.status === 'no_tariff') return 'Kein Tarif';
  if (price.status === 'km_pending') return `${formatEuro(price.total_cents)} + km (km ausstehend)`;
  return formatEuro(price.total_cents);
}

// Running trip with tracker data (booking.live): "Bisher 23 km · 41,50 € bei Rückgabe jetzt · Stand 11:05".
// The price is what the invoice says if the car is returned now; left out for club trips (never
// billed) and without a tariff. null without live data.
export function liveLine(live, nowSec, clubTrip = false) {
  if (!live) return null;
  const at = sameBerlinDay(live.as_of, nowSec) ? formatTime(live.as_of) : formatDateTime(live.as_of);
  const priced = !clubTrip && live.price && live.price.status === 'ok';
  const price = priced ? ` · ${formatEuro(live.price.total_cents)} bei Rückgabe jetzt` : '';
  return `Bisher ${live.km} km${price} · Stand ${at}`;
}

// Returned trip whose final km are not in yet: price and km from the readings so far, else null.
export function provisionalPrice(b) {
  if (b.club_trip || !b.km_pending || !b.live || !b.live.price || b.live.price.status !== 'ok') return null;
  return `${formatEuro(b.live.price.total_cents)} (vorläufiger Preis)`;
}

export function provisionalKm(b) {
  return b.km_pending && b.live ? `ca. ${b.live.km} km (vorläufig)` : null;
}

// 240 -> "4 h", 150 -> "2:30 h"
function hours(min) {
  const m = min % 60;
  return m === 0 ? `${min / 60} h` : `${Math.floor(min / 60)}:${String(m).padStart(2, '0')} h`;
}

// Per-line cents, half up. Display only: the total always comes from the server.
const activeCents = (min, rate) => Math.floor((min * rate + 30) / 60);
const unusedCents = (min, rate, factor) => Math.floor((min * rate * factor + 30000) / 60000);

export function priceLines(price) {
  const t = price.tariff;
  const lines = [];
  if (t) {
    const pctText = `${pct(t.unused_factor_permille)} %`;
    if (price.day_min) lines.push({ label: `${hours(price.day_min)} Tag × ${formatEuro(t.rate_day_cents_per_h)}`,
      cents: activeCents(price.day_min, t.rate_day_cents_per_h) });
    if (price.night_min) lines.push({ label: `${hours(price.night_min)} Nacht × ${formatEuro(t.rate_night_cents_per_h)}`,
      cents: activeCents(price.night_min, t.rate_night_cents_per_h) });
    if (price.unused_day_min) lines.push({
      label: `${hours(price.unused_day_min)} ungenutzt Tag × ${formatEuro(t.rate_day_cents_per_h)} × ${pctText}`,
      cents: unusedCents(price.unused_day_min, t.rate_day_cents_per_h, t.unused_factor_permille) });
    if (price.unused_night_min) lines.push({
      label: `${hours(price.unused_night_min)} ungenutzt Nacht × ${formatEuro(t.rate_night_cents_per_h)} × ${pctText}`,
      cents: unusedCents(price.unused_night_min, t.rate_night_cents_per_h, t.unused_factor_permille) });
    if (price.km_cents) lines.push({ label: `${price.km} km × ${formatEuro(t.rate_km_cents)}`, cents: price.km_cents });
  }
  const cut = price.time_cap_cut_cents + price.combined_cap_cut_cents;
  if (cut) lines.push({ label: 'Deckel', cents: -cut });
  if (price.late_fee_cents) lines.push({ label: 'Verspätung', cents: price.late_fee_cents });
  if (price.cancel_fee_cents) lines.push({ label: 'Stornogebühr', cents: price.cancel_fee_cents });
  return lines;
}

export function priceNote(tariff) {
  return `Nicht genutzte Zeit ${pct(tariff.unused_factor_permille)} %, Verspätung ${formatEuro(tariff.late_fee_cents_per_started_h)} je angefangene Stunde.`;
}

// Minutes after local midnight <-> "HH:MM"; null when invalid.
export function minutesToHHMM(min) {
  return `${String(Math.floor(min / 60)).padStart(2, '0')}:${String(min % 60).padStart(2, '0')}`;
}

export function hhmmToMinutes(text) {
  const m = /^(\d{1,2}):(\d{2})$/.exec(String(text).trim());
  if (!m || Number(m[1]) > 23 || Number(m[2]) > 59) return null;
  return Number(m[1]) * 60 + Number(m[2]);
}

// "75" / "75,5" -> permille (percent * 10), integer string math; null when invalid or > 100 %.
export function parsePercentToPermille(text) {
  const m = /^(\d{1,3})(?:[.,](\d))?$/.exec(String(text).trim());
  if (!m) return null;
  const p = Number(m[1]) * 10 + Number(m[2] || 0);
  return p <= 1000 ? p : null;
}

// One-line tariff description (admin list), without the valid_from prefix.
export function tariffSummary(t) {
  return `Tag ${formatEuro(t.rate_day_cents_per_h)}/h, Nacht ${formatEuro(t.rate_night_cents_per_h)}/h ` +
    `(${minutesToHHMM(t.night_start)}–${minutesToHHMM(t.night_end)}), ${formatEuro(t.rate_km_cents)}/km, ` +
    `ungenutzt ${pct(t.unused_factor_permille)} %, Deckel ${formatEuro(t.time_cap_cents_per_day)} / ` +
    `${formatEuro(t.combined_cap_cents_per_day)} pro Tag, Verspätung ${formatEuro(t.late_fee_cents_per_started_h)}/h`;
}
