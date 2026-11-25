// Service worker: app shell cache-first with background update; /api/* is never cached.
// Keep SHELL in sync with `git ls-files web` (minus web/test/); bump CACHE on every web change.
const CACHE = 'ocs-v35';
const SHELL = [
  '/',
  '/index.html',
  '/help.html',
  '/style.css',
  '/manifest.json',
  '/icon.svg',
  '/icon-192.png',
  '/icon-512.png',
  '/img/logo.svg',
  '/img/logo-dark.svg',
  '/img/logo-full.svg',
  '/img/logo-full-dark.svg',
  '/js/admin_ui.js',
  '/js/api.js',
  '/js/app.js',
  '/js/billing.js',
  '/js/booking_card.js',
  '/js/form_time.js',
  '/js/icons.js',
  '/js/money.js',
  '/js/photo.js',
  '/js/price.js',
  '/js/push.js',
  '/js/sepa.js',
  '/js/time.js',
  '/js/trip.js',
  '/js/views/account.js',
  '/js/views/admin_billing.js',
  '/js/views/admin_bookings.js',
  '/js/views/admin_cars.js',
  '/js/views/admin_settings.js',
  '/js/views/admin_tariffs.js',
  '/js/views/admin_users.js',
  '/js/views/booking_form.js',
  '/js/views/calendar.js',
  '/js/views/damage_dialog.js',
  '/js/views/history.js',
  '/js/views/login.js',
  '/js/views/my_bookings.js',
  '/js/views/pin_dialog.js',
  '/js/views/password.js',
];

self.addEventListener('install', (event) => {
  event.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => k.startsWith('ocs-') && k !== CACHE).map((k) => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== self.location.origin || url.pathname.startsWith('/api/')) return;
  event.respondWith((async () => {
    const cache = await caches.open(CACHE);
    // App routes live in the hash, so '/' and '/index.html' are one shell; other pages (help.html) keep their own key.
    const key = req.mode === 'navigate' ? (url.pathname === '/index.html' ? '/' : url.pathname) : req;
    const cached = await cache.match(key);
    const update = fetch(req).then((res) => {
      if (res.ok) cache.put(key, res.clone());
      return res;
    });
    if (cached) { update.catch(() => {}); return cached; }
    return update;
  })());
});

// Web Push (spec §10): the server sends {title, body, url, tag}, encrypted end to end.
self.addEventListener('push', (event) => {
  let d = {};
  try { d = event.data ? event.data.json() : {}; } catch { d = { body: event.data ? event.data.text() : '' }; }
  event.waitUntil(self.registration.showNotification(d.title || 'Carsharing', {
    body: d.body || '',
    tag: d.tag || undefined,
    renotify: !!d.tag,
    icon: '/icon-192.png',
    data: { url: d.url || '/#/buchungen' },
  }));
});

// Tap: bring an open app window to the target view, else open one. Same origin only.
self.addEventListener('notificationclick', (event) => {
  event.notification.close();
  let url = new URL((event.notification.data && event.notification.data.url) || '/', self.location.origin);
  if (url.origin !== self.location.origin) url = new URL('/', self.location.origin);
  event.waitUntil((async () => {
    const wins = await self.clients.matchAll({ type: 'window', includeUncontrolled: true });
    for (const w of wins) {
      if (new URL(w.url).origin !== self.location.origin) continue;
      await w.focus();
      if ('navigate' in w) await w.navigate(url.href).catch(() => {});
      return;
    }
    await self.clients.openWindow(url.href);
  })());
});
