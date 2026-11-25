// Shell: hash router, session state, top bar, menu and mobile tab bar.
import { api, onUnauthorized } from './api.js';
import { icon } from './icons.js';
import { syncPush, dropPushOnLogout } from './push.js';

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
  '#/admin/tarife': 'admin_tariffs',
  '#/admin/buchungen': 'admin_bookings',
  '#/admin/abrechnung': 'admin_billing',
  '#/admin/einstellungen': 'admin_settings',
};
const PUBLIC = new Set(['#/login', '#/passwort']);
const DEFAULT_ROUTE = '#/kalender';
// Destinations in the mobile tab bar; the menu shows them only on desktop (class in-tabs).
const TABS = [
  ['#/kalender', 'Kalender', 'calendar'],
  ['#/buchungen', 'Buchungen', 'list'],
  ['#/verlauf', 'Verlauf', 'clock'],
  ['#/konto', 'Konto', 'user'],
];

if ('serviceWorker' in navigator) navigator.serviceWorker.register('/sw.js').catch(() => {});

let user = null;
let carName = '';
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
const tabbarEl = document.getElementById('tabbar');
const topbarEl = document.getElementById('topbar');
let viewEl = document.getElementById('view');
let moreBtn = null;

function setMenuOpen(open) {
  menuEl.hidden = !open;
  if (moreBtn) moreBtn.setAttribute('aria-expanded', String(open));
}

// Marks the current destination in menu and tab bar; any admin route lights up "Admin".
function markCurrent() {
  const { path } = parseHash(location.hash);
  for (const a of [...menuEl.querySelectorAll('a'), ...tabbarEl.querySelectorAll('a')]) {
    const href = a.getAttribute('href');
    const current = href === path || (href === '#/admin/nutzer' && path.startsWith('#/admin/'));
    if (current) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  }
}

function updateChrome() {
  topbarEl.hidden = !user;
  tabbarEl.hidden = !user;
  document.body.classList.toggle('tabs', !!user);
  document.getElementById('car-name').textContent = carName;
  moreBtn = null;
  setMenuOpen(false);
  menuEl.replaceChildren();
  tabbarEl.replaceChildren();
  if (!user) return;
  const inTabs = new Set(TABS.map(([href]) => href));
  const items = TABS.map(([href, label]) => [href, label === 'Buchungen' ? 'Meine Buchungen' : label]);
  if (user.role === 'admin') items.push(['#/admin/nutzer', 'Admin']);
  items.push(['/help.html', 'Hilfe']);
  for (const [href, label] of items) {
    const a = document.createElement('a');
    a.href = href;
    a.textContent = label;
    if (inTabs.has(href)) a.className = 'in-tabs';
    menuEl.append(a);
  }
  const out = document.createElement('button');
  out.type = 'button';
  out.textContent = 'Abmelden';
  out.addEventListener('click', logout);
  menuEl.append(out);

  for (const [href, label, ic] of TABS) {
    const a = document.createElement('a');
    a.className = 'tab';
    a.href = href;
    const text = document.createElement('span');
    text.textContent = label;
    a.append(icon(ic), text);
    tabbarEl.append(a);
  }
  moreBtn = document.createElement('button');
  moreBtn.type = 'button';
  moreBtn.className = 'tab';
  moreBtn.setAttribute('aria-expanded', 'false');
  moreBtn.setAttribute('aria-controls', 'menu');
  const moreText = document.createElement('span');
  moreText.textContent = 'Mehr';
  moreBtn.append(icon('more'), moreText);
  moreBtn.addEventListener('click', () => setMenuOpen(menuEl.hidden));
  tabbarEl.append(moreBtn);
  markCurrent();
}

async function logout() {
  await dropPushOnLogout(); /* needs the session, so before /api/logout */
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
  markCurrent();
  let mod = null;
  try {
    mod = await import(`./views/${name}.js`);
  } catch { /* view not written yet */ }
  if (token !== renderToken) return;
  // Fresh element per route: a render or in-view reload still running for an earlier
  // route keeps its old element, which is detached now, so its late output never shows.
  const root = document.createElement('main');
  root.id = 'view';
  root.className = 'view';
  viewEl.replaceWith(root);
  viewEl = root;
  if (!mod || typeof mod.render !== 'function') return showPlaceholder();
  try {
    await mod.render(root, params);
  } catch (e) {
    const p = document.createElement('p');
    p.className = 'error';
    p.textContent = (e && e.message) || 'Es ist ein Fehler aufgetreten.';
    root.replaceChildren(p);
  }
}

menuEl.addEventListener('click', (e) => { if (e.target.tagName === 'A') setMenuOpen(false); });
// Tap outside the open sheet closes it (desktop shows the menu inline; hidden is ignored there).
document.addEventListener('click', (e) => {
  if (menuEl.hidden || menuEl.contains(e.target) || (moreBtn && moreBtn.contains(e.target))) return;
  setMenuOpen(false);
});
window.addEventListener('hashchange', route);

onUnauthorized(() => {
  user = null;
  updateChrome();
  // Public routes (login, password reset/invite) are never redirected.
  if (!PUBLIC.has(parseHash(location.hash).path)) navigate('#/login');
});

await refreshUser();
if (user) syncPush();
if (!location.hash) location.hash = user ? DEFAULT_ROUTE : '#/login';
else route();
