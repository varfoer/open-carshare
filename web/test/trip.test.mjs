import { test } from 'node:test'; import assert from 'node:assert/strict';
import { tripActions, damageTextTooLong, pinView } from '../js/trip.js';
import { fitSize } from '../js/photo.js';
test('tripActions', () => {
  const b = { state: 'reserved', booked_start: 1000, booked_end: 2000 };
  assert.deepEqual(tripActions(b, 900), { start: true, ret: false, report: false, extend: true, cancel: true });
  assert.deepEqual(tripActions(b, 1500), { start: true, ret: false, report: false, extend: true, cancel: false });
  assert.deepEqual(tripActions(b, 2000), { start: false, ret: false, report: false, extend: true, cancel: false });
  assert.deepEqual(tripActions({ ...b, state: 'active' }, 2500), { start: false, ret: true, report: true, extend: true, cancel: false });
});
test('fitSize', () => {
  assert.deepEqual(fitSize(4000, 3000), { w: 1600, h: 1200 });
  assert.deepEqual(fitSize(3000, 4000), { w: 1200, h: 1600 });
  assert.deepEqual(fitSize(800, 600), { w: 800, h: 600 });
  assert.deepEqual(fitSize(1601, 1), { w: 1600, h: 1 });
});
test('damageTextTooLong counts UTF-8 bytes', () => {
  assert.equal(damageTextTooLong('a'.repeat(1023)), false);
  assert.equal(damageTextTooLong('a'.repeat(1024)), true);
  assert.equal(damageTextTooLong('ä'.repeat(511) + 'a'), false); // 1023 bytes
  assert.equal(damageTextTooLong('ä'.repeat(512)), true); // 512 UTF-16 units, 1024 bytes
  assert.equal(damageTextTooLong('🚗'.repeat(256)), true); // 4 bytes each
});
test('pinView', () => {
  assert.deepEqual(pinView({ pin: '5729314', valid_from: 0, valid_until: 10000 }, 9099), { kind: 'show', pin: '5729314', until: 10000 });
  assert.deepEqual(pinView({ pin: '5729314', valid_from: 0, valid_until: 10000 }, 9100), { kind: 'expired' });
  assert.deepEqual(pinView({ pin: null, valid_from: 0, valid_until: 10000 }, 0), { kind: 'invalid' });
  assert.deepEqual(pinView({ pin: null, valid_from: 0, valid_until: 10000 }, 9099), { kind: 'invalid' });
  assert.deepEqual(pinView({ pin: null, valid_from: 0, valid_until: 10000 }, 9100), { kind: 'expired' });
  assert.deepEqual(pinView({ pin: null, valid_from: 0, valid_until: 0 }, 0), { kind: 'none' });
});
