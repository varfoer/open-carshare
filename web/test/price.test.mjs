import test from 'node:test';
import assert from 'node:assert/strict';
import { priceSummary, priceLines, priceNote } from '../js/price.js';

const TARIFF = { id: 1, car_id: 1, valid_from: 0, rate_day_cents_per_h: 300, rate_night_cents_per_h: 150,
  rate_km_cents: 30, night_start: 1200, night_end: 360, unused_factor_permille: 500,
  time_cap_cents_per_day: 3000, combined_cap_cents_per_day: 5000, late_fee_cents_per_started_h: 1500, created_at: 0 };

function price(o) {
  return { status: 'ok', day_min: 0, night_min: 0, unused_day_min: 0, unused_night_min: 0, days: 1,
    time_cents: 0, time_cap_cut_cents: 0, km: 0, km_cents: 0, combined_cap_cut_cents: 0, late_min: 0,
    late_fee_cents: 0, cancel_fee_cents: 0, total_cents: 0, tariff: TARIFF, ...o };
}

test('priceSummary', () => {
  assert.equal(priceSummary(price({ total_cents: 1250 })), '12,50 €');
  assert.equal(priceSummary(price({ status: 'km_pending', total_cents: 600 })), '6,00 € + km (km ausstehend)');
  assert.equal(priceSummary(price({ status: 'no_tariff', tariff: null })), 'Kein Tarif');
});

test('priceLines afternoon', () => {
  const l = priceLines(price({ day_min: 240, km: 20, km_cents: 600 }));
  assert.deepEqual(l, [
    { label: '4 h Tag × 3,00 €', cents: 1200 },
    { label: '20 km × 0,30 €', cents: 600 },
  ]);
});

test('priceLines full day with caps', () => {
  const l = priceLines(price({ day_min: 840, night_min: 600, km: 200, km_cents: 6000,
    time_cap_cut_cents: 2700, combined_cap_cut_cents: 4000 }));
  assert.deepEqual(l.map((x) => x.label), ['14 h Tag × 3,00 €', '10 h Nacht × 1,50 €', '200 km × 0,30 €', 'Deckel']);
  assert.deepEqual(l.map((x) => x.cents), [4200, 1500, 6000, -6700]);
});

test('priceLines early return unused', () => {
  const l = priceLines(price({ day_min: 60, unused_day_min: 150, unused_night_min: 60 }));
  assert.deepEqual(l, [
    { label: '1 h Tag × 3,00 €', cents: 300 },
    { label: '2:30 h ungenutzt Tag × 3,00 € × 50 %', cents: 375 },
    { label: '1 h ungenutzt Nacht × 1,50 € × 50 %', cents: 75 },
  ]);
});

test('priceLines cancelled', () => {
  const l = priceLines(price({ cancel_fee_cents: 500, total_cents: 500, tariff: null }));
  assert.deepEqual(l, [{ label: 'Stornogebühr', cents: 500 }]);
});

test('priceLines 90 min night, late fee, rounding', () => {
  const l = priceLines(price({ night_min: 90, late_min: 5, late_fee_cents: 1500 }));
  assert.deepEqual(l, [
    { label: '1:30 h Nacht × 1,50 €', cents: 225 },
    { label: 'Verspätung', cents: 1500 },
  ]);
  assert.equal(priceLines(price({ day_min: 1 }))[0].cents, 5);
  assert.equal(priceLines(price({ day_min: 10 }))[0].cents, 50);
});

test('priceLines skips rate lines without tariff', () => {
  assert.deepEqual(priceLines(price({ day_min: 60, tariff: null })), []);
});

test('priceNote', () => {
  assert.equal(priceNote(TARIFF), 'Nicht genutzte Zeit 50 %, Verspätung 15,00 € je angefangene Stunde.');
});
