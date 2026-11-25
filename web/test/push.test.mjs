import { test } from 'node:test';
import assert from 'node:assert/strict';
import { b64urlToBytes, bytesToB64url, sameKey, subscriptionBody, pushSupport } from '../js/push.js';

const KEY = 'BCVxsr7N_eNgVRqvHtD0zTZsEc6-VV-JvLexhqUzORcxaOzi6-AYWXvTBHm4bjyPjs7Vd8pZGH6SRpkNtoIAiw4';

test('base64url round trip of a server key', () => {
  const b = b64urlToBytes(KEY);
  assert.equal(b.length, 65);
  assert.equal(b[0], 4);
  assert.equal(bytesToB64url(b), KEY);
  assert.equal(bytesToB64url(b.buffer), KEY);
  assert.deepEqual([...b64urlToBytes('-_8')], [0xfb, 0xff]);
});

test('sameKey compares the subscription key with the server key', () => {
  assert.equal(sameKey(b64urlToBytes(KEY).buffer, KEY), true);
  assert.equal(sameKey(b64urlToBytes(KEY).buffer, KEY.replace('BCV', 'BCW')), false);
  assert.equal(sameKey(null, KEY), false);
});

test('subscriptionBody takes endpoint and keys, drops padding', () => {
  assert.deepEqual(subscriptionBody({ endpoint: 'https://fcm.googleapis.com/fcm/send/x', keys: { p256dh: 'AAA=', auth: 'BB==' } }),
    { endpoint: 'https://fcm.googleapis.com/fcm/send/x', p256dh: 'AAA', auth: 'BB' });
});

test('pushSupport: supported, iPhone Safari tab, other', () => {
  const win = { PushManager: {}, Notification: {}, matchMedia: () => ({ matches: false }) };
  assert.equal(pushSupport({ serviceWorker: {}, userAgent: 'Android' }, win), 'ok');
  const iphone = { userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X)', serviceWorker: {} };
  assert.equal(pushSupport(iphone, { matchMedia: () => ({ matches: false }) }), 'ios-install');
  assert.equal(pushSupport({ ...iphone, standalone: true }, { matchMedia: () => ({ matches: true }) }), 'unsupported');
  assert.equal(pushSupport({ userAgent: 'Old' }, { matchMedia: () => ({ matches: false }) }), 'unsupported');
});
