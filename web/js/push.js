// Web Push on this device (spec §10): state, enable/disable, sync at app start, drop on logout.
// With push on, the reminders (start, end, late return) come as push instead of mail.
import { api } from './api.js';

export function b64urlToBytes(s) {
  const b64 = s.replace(/-/g, '+').replace(/_/g, '/');
  const bin = atob(b64 + '='.repeat((4 - (b64.length % 4)) % 4));
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
}

export function bytesToB64url(bytes) {
  let bin = '';
  for (const b of new Uint8Array(bytes)) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

// Was the subscription made with the server's current key? (key lost or replaced -> resubscribe)
export function sameKey(applicationServerKey, b64) {
  return !!applicationServerKey && bytesToB64url(applicationServerKey) === b64;
}

// The fields POST /api/me/push expects, from PushSubscription.toJSON().
export function subscriptionBody(json) {
  const strip = (s) => String(s || '').replace(/=+$/, '');
  return { endpoint: json.endpoint, p256dh: strip(json.keys && json.keys.p256dh), auth: strip(json.keys && json.keys.auth) };
}

// 'ok', 'ios-install' (iPhone/iPad Safari tab: push only for the app on the home screen) or 'unsupported'.
export function pushSupport(nav = navigator, win = window) {
  if ('serviceWorker' in nav && 'PushManager' in win && 'Notification' in win) return 'ok';
  const ios = /iPad|iPhone|iPod/.test(nav.userAgent) || (nav.platform === 'MacIntel' && nav.maxTouchPoints > 1);
  const standalone = nav.standalone === true || (win.matchMedia && win.matchMedia('(display-mode: standalone)').matches);
  return ios && !standalone ? 'ios-install' : 'unsupported';
}

async function registration() {
  const reg = await navigator.serviceWorker.getRegistration();
  if (!reg) throw new Error('Die App ist noch nicht vollständig geladen. Bitte die Seite neu laden.');
  return reg;
}

// 'ios-install' | 'unsupported' | 'denied' | 'on' | 'off'
export async function pushState() {
  const s = pushSupport();
  if (s !== 'ok') return s;
  if (Notification.permission === 'denied') return 'denied';
  const reg = await navigator.serviceWorker.getRegistration();
  const sub = reg && await reg.pushManager.getSubscription();
  return sub && Notification.permission === 'granted' ? 'on' : 'off';
}

// Subscribes with the server key (renewing a subscription made with an old key) and stores it on the server.
async function register(reg) {
  const { key } = await api('GET', '/api/push/key');
  let sub = await reg.pushManager.getSubscription();
  if (sub && !sameKey(sub.options.applicationServerKey, key)) {
    await sub.unsubscribe();
    sub = null;
  }
  if (!sub) {
    try {
      sub = await reg.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: b64urlToBytes(key) });
    } catch {
      throw new Error('Push-Nachrichten konnten auf diesem Gerät nicht eingerichtet werden.');
    }
  }
  await api('POST', '/api/me/push', subscriptionBody(sub.toJSON()));
}

export async function enablePush() {
  const p = await Notification.requestPermission();
  if (p === 'denied') throw new Error('Benachrichtigungen sind für diese Seite blockiert. Erlaube sie in den Einstellungen des Browsers.');
  if (p !== 'granted') throw new Error('Benachrichtigungen wurden nicht erlaubt.');
  await register(await registration());
}

export async function disablePush() {
  const reg = await registration();
  const sub = await reg.pushManager.getSubscription();
  if (!sub) return;
  await api('DELETE', '/api/me/push', { endpoint: sub.endpoint });
  await sub.unsubscribe();
}

// After login / at app start: re-sends this device's subscription, so the server has the current
// owner, endpoint and key. Does nothing when push is off on this device. Never throws.
export async function syncPush() {
  try {
    if (pushSupport() !== 'ok' || Notification.permission !== 'granted') return;
    const reg = await navigator.serviceWorker.getRegistration();
    if (!reg || !await reg.pushManager.getSubscription()) return;
    await register(reg);
  } catch { /* next app start tries again */ }
}

// Before logout: the device stops getting this member's reminders. Never throws.
export async function dropPushOnLogout() {
  try {
    if (pushSupport() !== 'ok') return;
    const reg = await navigator.serviceWorker.getRegistration();
    const sub = reg && await reg.pushManager.getSubscription();
    if (!sub) return;
    await api('DELETE', '/api/me/push', { endpoint: sub.endpoint }).catch(() => {});
    await sub.unsubscribe();
  } catch { /* logged out anyway */ }
}
