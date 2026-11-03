import { test } from 'node:test'; import assert from 'node:assert/strict';
import { lastMonth, periodLabel, dateDe, blockerLines } from '../js/billing.js';
test('lastMonth uses Berlin month', () => {
  assert.equal(lastMonth(new Date('2026-11-01T00:30:00+01:00')), '2026-10');
  assert.equal(lastMonth(new Date('2026-10-31T23:30:00Z')), '2026-10'); // already Nov 1 in Berlin
  assert.equal(lastMonth(new Date('2026-01-15T12:00:00Z')), '2025-12');
});
test('periodLabel', () => {
  assert.equal(periodLabel('2026-03'), 'März 2026');
  assert.equal(periodLabel('2026-10'), 'Oktober 2026');
});
test('dateDe', () => assert.equal(dateDe('2026-11-09'), '09.11.2026'));
test('blockerLines', () => {
  const lines = blockerLines({ km_pending: [12], no_tariff: [7], no_mandate: [3] },
    new Map([[3, { name: 'Anna B.' }]]));
  assert.deepEqual(lines, ['Buchung 12: km fehlen', 'Buchung 7: kein Tarif', 'Anna B.: kein SEPA-Mandat']);
  assert.deepEqual(blockerLines({ km_pending: [], no_tariff: [], no_mandate: [] }, new Map()), []);
  assert.deepEqual(blockerLines({ no_mandate: [9] }, { 9: { name: 'X' } }), ['X: kein SEPA-Mandat']);
});
