import { test } from 'node:test'; import assert from 'node:assert/strict';
import { berlinToEpoch, epochToBerlin, formatDateTime, formatTime, startOfBerlinDay } from '../js/time.js';
test('winter time', () => assert.equal(berlinToEpoch(2026,3,29,1,0), 1774742400));
test('summer time', () => assert.equal(berlinToEpoch(2026,3,29,3,0), 1774746000));
test('dst gap shifts forward', () => assert.equal(berlinToEpoch(2026,3,29,2,30), 1774747800));
test('dst overlap takes first', () => assert.equal(berlinToEpoch(2026,10,25,2,30), 1792888200));
test('roundtrip', () => assert.deepEqual(
  (({y,mo,d,h,mi}) => ({y,mo,d,h,mi}))(epochToBerlin(1774746000)), {y:2026,mo:3,d:29,h:3,mi:0}));
test('format', () => assert.equal(formatDateTime(1774746000), 'So, 29.03.2026, 03:00'));
test('weekday number', () => assert.equal(epochToBerlin(1774746000).wd, 7));
test('formatTime', () => assert.equal(formatTime(1774746000), '03:00'));
test('startOfBerlinDay', () => {
  assert.equal(startOfBerlinDay(1774746000), 1774738800); // 29.03. 00:00 CET
  assert.equal(startOfBerlinDay(1774746000 + 86400), 1774738800 + 23 * 3600); // 30.03. 00:00 CEST
});
