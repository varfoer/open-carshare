import test from 'node:test';
import assert from 'node:assert/strict';
import { parseEuroToCents, formatEuro } from '../js/money.js';
import { monthRange, berlinToEpoch } from '../js/time.js';

test('parseEuroToCents accepts comma and dot', () => {
  assert.equal(parseEuroToCents('5'), 500);
  assert.equal(parseEuroToCents('5,00'), 500);
  assert.equal(parseEuroToCents('5.5'), 550);
  assert.equal(parseEuroToCents(' 0,07 '), 7);
  assert.equal(parseEuroToCents('19,99'), 1999);
  assert.equal(parseEuroToCents('1.15'), 115);
  assert.equal(parseEuroToCents('0'), 0);
});

test('parseEuroToCents rejects invalid input', () => {
  for (const v of ['', 'abc', '-1', '5,555', '5,', ',5', '1,000.00', '5 €', '1e3', '5,5,5'])
    assert.equal(parseEuroToCents(v), null, v);
});

test('formatEuro negative', () => {
  assert.equal(formatEuro(-6700), '-67,00 €');
  assert.equal(formatEuro(-6750), '-67,50 €');
  assert.equal(formatEuro(-5), '-0,05 €');
});

test('formatEuro round trip', () => {
  assert.equal(formatEuro(500), '5,00 €');
  assert.equal(formatEuro(7), '0,07 €');
  assert.equal(parseEuroToCents(formatEuro(1999).replace(' €', '')), 1999);
});

test('monthRange uses Berlin midnight, incl. DST months and year wrap', () => {
  const m = monthRange(2026, 3); // CET -> CEST during March
  assert.equal(m.from, berlinToEpoch(2026, 3, 1, 0, 0));
  assert.equal(m.to, berlinToEpoch(2026, 4, 1, 0, 0));
  assert.equal(m.to - m.from, 31 * 86400 - 3600);
  assert.equal(monthRange(2026, 12).to, berlinToEpoch(2027, 1, 1, 0, 0));
  assert.equal(monthRange(2026, 10).to - monthRange(2026, 10).from, 31 * 86400 + 3600);
});
