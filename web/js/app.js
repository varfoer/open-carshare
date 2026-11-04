// Shell: hash router, session state, top bar and menu.
import { api, onUnauthorized } from './api.js';

const ROUTES = {
  '#/login': 'login',
  '#/passwort': 'password',
  '#/kalender': 'calendar',
  '#/buchen': 'booking_form',
  '#/buchungen': 'my_bookings',
  '#/verlauf': 'history',
  '#/konto': 'account',
  '#/admin/nutzer': 'admin_users',
  '#/admin/autos': 'admin_cars',
  '#/admin/buchungen': 'admin_bookings',
  '#/admin/abrechnung': 'admin_billing',
  '#/admin/einstellungen': 'admin_settings',
};
const PUBLIC = new Set(['#/login', '#/passwort']);
const DEFAULT_ROUTE = '#/kalender';

if ('serviceWorker' in navigator) navigator.serviceWorker.register('/sw.js').catch(() => {});

let user = null;
let carName = 'Carsharing';
let renderToken = 0;

export function currentUser() { return user; }

export function navigate(hash) {
  if (location.hash === hash) route();
  else location.hash = hash;
}

// Re-reads the session from the server; returns the user or null.
export async function refreshUser() {
  try {
    user = (await api('GET', '/api/me')).user;
  } catch {
    user = null;
  }
  if (user) await loadCarName();
  updateChrome();
  return user;
}

async function loadCarName() {
  try {
    const cars = await api('GET', '/api/cars');
    if (Array.isArray(cars) && cars.length) carName = cars[0].name;
  } catch { /* keep default */ }
}

export function parseHash(hash) {
  const [path, q = ''] = hash.split('?');
  return { path, params: Object.fromEntries(new URLSearchParams(q)) };
}

const menuEl = document.getElementById('menu');
const toggleEl = document.getElementById('menu-toggle');
const topbarEl = document.getElementById('topbar');
const viewEl = document.getElementById('view');

function setMenuOpen(open) {
  menuEl.hidden = !open;
  toggleEl.setAttribute('aria-expanded', String(open));
}

function updateChrome() {
  topbarEl.hidden = !user;
  document.getElementById('car-name').textContent = carName;
  setMenuOpen(false);
  menuEl.replaceChildren();
  if (!user) return;
  const items = [
    ['#/kalender', 'Kalender'],
    ['#/buchungen', 'Meine Buchungen'],
    ['#/verlauf', 'Verlauf'],
    ['#/konto', 'Konto'],
  ];
  if (user.role === 'admin') items.push(['#/admin/nutzer', 'Admin']);
  items.push(['/help.html', 'Hilfe']);
  for (const [href, label] of items) {
    const a = document.createElement('a');
    a.href = href;
    a.textContent = label;
    menuEl.append(a);
  }
  const out = document.createElement('button');
  out.type = 'button';
  out.textContent = 'Abmelden';
  out.addEventListener('click', logout);
  menuEl.append(out);
}

async function logout() {
  try { await api('POST', '/api/logout'); } catch { /* session gone anyway */ }
  user = null;
  updateChrome();
  navigate('#/login');
}

function showPlaceholder() {
  const p = document.createElement('p');
  p.className = 'placeholder';
  p.textContent = 'Diese Ansicht ist noch nicht verfügbar.';
  viewEl.replaceChildren(p);
}

async function route() {
  const token = ++renderToken;
  const { path, params } = parseHash(location.hash);
  const name = ROUTES[path];
  if (!name) return navigate(user ? DEFAULT_ROUTE : '#/login');
  if (!PUBLIC.has(path) && !user) return navigate('#/login');
  if (path === '#/login' && user) return navigate(DEFAULT_ROUTE);
  setMenuOpen(false);
  let mod = null;
  try {
    mod = await import(`./views/${name}.js`);
  } catch { /* view not written yet */ }
  if (token !== renderToken) return;
  viewEl.replaceChildren();
  viewEl.className = 'view';
  if (!mod || typeof mod.render !== 'function') return showPlaceholder();
  try {
    await mod.render(viewEl, params);
  } catch (e) {
    if (token !== renderToken) return;
    const p = document.createElement('p');
    p.className = 'error';
    p.textContent = (e && e.message) || 'Es ist ein Fehler aufgetreten.';
    viewEl.replaceChildren(p);
  }
}

toggleEl.addEventListener('click', () => setMenuOpen(menuEl.hidden));
menuEl.addEventListener('click', (e) => { if (e.target.tagName === 'A') setMenuOpen(false); });
window.addEventListener('hashchange', route);

onUnauthorized(() => {
  user = null;
  updateChrome();
  // Public routes (login, password reset/invite) are never redirected.
  if (!PUBLIC.has(parseHash(location.hash).path)) navigate('#/login');
});

await refreshUser();
if (!location.hash) location.hash = user ? DEFAULT_ROUTE : '#/login';
else route();
