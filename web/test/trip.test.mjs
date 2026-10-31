import { test } from 'node:test'; import assert from 'node:assert/strict';
import { tripActions } from '../js/trip.js';
import { fitSize } from '../js/photo.js';
test('tripActions', () => {
  const b = { state: 'reserved', booked_start: 1000, booked_end: 2000 };
  assert.deepEqual(tripActions(b, 900), { start: true, ret: false, extend: true, cancel: true });
  assert.deepEqual(tripActions(b, 1500), { start: true, ret: false, extend: true, cancel: false });
  assert.deepEqual(tripActions(b, 2000), { start: false, ret: false, extend: true, cancel: false });
  assert.deepEqual(tripActions({ ...b, state: 'active' }, 2500), { start: false, ret: true, extend: true, cancel: false });
});
test('fitSize', () => {
  assert.deepEqual(fitSize(4000, 3000), { w: 1600, h: 1200 });
  assert.deepEqual(fitSize(3000, 4000), { w: 1200, h: 1600 });
  assert.deepEqual(fitSize(800, 600), { w: 800, h: 600 });
  assert.deepEqual(fitSize(1601, 1), { w: 1600, h: 1 });
});
