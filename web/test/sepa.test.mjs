import { test } from 'node:test'; import assert from 'node:assert/strict';
import { normalizeIban, ibanValid, formatIban } from '../js/sepa.js';
test('normalize', () => assert.equal(normalizeIban(' de89 3704 '), 'DE893704'));
test('valid', () => {
  assert.equal(ibanValid('DE89 3704 0044 0532 0130 00'), true);
  assert.equal(ibanValid('GB82WEST12345698765432'), true);
});
test('invalid', () => {
  assert.equal(ibanValid('DE89370400440532013001'), false);
  assert.equal(ibanValid('DE89'), false);
  assert.equal(ibanValid(''), false);
});
test('format', () => assert.equal(formatIban('DE89370400440532013000'), 'DE89 3704 0044 0532 0130 00'));
