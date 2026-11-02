// Price display for the server's booking `price` object. Money stays integer cents.
import { formatEuro } from './money.js';

export function priceSummary(price) {
  if (price.status === 'no_tariff') return 'Kein Tarif';
  if (price.status === 'km_pending') return `${formatEuro(price.total_cents)} + km (km ausstehend)`;
  return formatEuro(price.total_cents);
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
    const pct = `${t.unused_factor_permille / 10} %`;
    if (price.day_min) lines.push({ label: `${hours(price.day_min)} Tag × ${formatEuro(t.rate_day_cents_per_h)}`,
      cents: activeCents(price.day_min, t.rate_day_cents_per_h) });
    if (price.night_min) lines.push({ label: `${hours(price.night_min)} Nacht × ${formatEuro(t.rate_night_cents_per_h)}`,
      cents: activeCents(price.night_min, t.rate_night_cents_per_h) });
    if (price.unused_day_min) lines.push({
      label: `${hours(price.unused_day_min)} ungenutzt Tag × ${formatEuro(t.rate_day_cents_per_h)} × ${pct}`,
      cents: unusedCents(price.unused_day_min, t.rate_day_cents_per_h, t.unused_factor_permille) });
    if (price.unused_night_min) lines.push({
      label: `${hours(price.unused_night_min)} ungenutzt Nacht × ${formatEuro(t.rate_night_cents_per_h)} × ${pct}`,
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
  return `Nicht genutzte Zeit ${tariff.unused_factor_permille / 10} %, Verspätung ${formatEuro(tariff.late_fee_cents_per_started_h)} je angefangene Stunde.`;
}
