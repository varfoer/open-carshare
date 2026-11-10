// Small DOM helpers shared by account and admin views. All data via textContent.
import { icon } from './icons.js';

export function el(tag, props = {}, ...children) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'text') n.textContent = v;
    else if (k in n) n[k] = v;
    else n.setAttribute(k, v);
  }
  n.append(...children);
  return n;
}

const ADMIN_SECTIONS = [
  ['#/admin/nutzer', 'Nutzer', 'users'], ['#/admin/autos', 'Autos', 'car'], ['#/admin/tarife', 'Tarife', 'tag'],
  ['#/admin/buchungen', 'Buchungen', 'calendar'], ['#/admin/abrechnung', 'Abrechnung', 'receipt'],
  ['#/admin/einstellungen', 'Einstellungen', 'settings'],
];

export function adminNav(active) {
  const nav = el('nav', { className: 'subnav' }, el('span', { className: 'subnav-title', text: 'Verwaltung' }));
  nav.setAttribute('aria-label', 'Verwaltung');
  for (const [href, label, ic] of ADMIN_SECTIONS) {
    const a = el('a', { href }, icon(ic), el('span', { text: label }));
    if (href === active) a.setAttribute('aria-current', 'page');
    nav.append(a);
  }
  return nav;
}

// Admin page frame: section nav (sidebar on desktop) plus the main column the view fills.
export function adminPage(root, active) {
  root.className = 'view admin';
  const main = el('div', { className: 'admin-main' });
  root.append(adminNav(active), main);
  return main;
}

// Page header: title, optional subtitle line, action buttons on the right.
export function pageHead(title, sub, ...actions) {
  const head = el('div', { className: 'page-head' }, el('div', {}, el('h1', { text: title })));
  if (sub) head.firstChild.append(el('span', { className: 'sub', text: sub }));
  if (actions.length) head.append(el('div', { className: 'actions' }, ...actions));
  return head;
}

export function messageBox(kind) {
  const p = el('p', { className: kind });
  p.setAttribute('role', kind === 'error' ? 'alert' : 'status');
  p.hidden = true;
  return {
    node: p,
    show(text) { p.textContent = text; p.hidden = false; },
    hide() { p.hidden = true; },
  };
}

export function textField(label, value = '', attrs = {}) {
  const input = el('input', { type: 'text', value, ...attrs });
  return { input, node: el('label', {}, label, input) };
}

export function checkField(label, checked) {
  const input = el('input', { type: 'checkbox', checked });
  const node = el('label', { className: 'check' }, input, ` ${label}`);
  return { input, node };
}

export function selectField(label, options, value) {
  const select = el('select');
  for (const [v, t] of options) select.append(el('option', { value: String(v), text: t }));
  if (value !== undefined) select.value = String(value);
  return { input: select, node: el('label', {}, label, select) };
}

export function primaryButton(text) {
  return el('button', { type: 'submit', className: 'primary', text });
}

export function badge(text, kind = '') {
  return el('span', { className: kind ? `badge ${kind}` : 'badge', text });
}
