// Small DOM helpers shared by account and admin views. All data via textContent.
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

export function adminNav(active) {
  const nav = el('nav', { className: 'subnav' });
  for (const [href, label] of [
    ['#/admin/nutzer', 'Nutzer'], ['#/admin/autos', 'Autos'],
    ['#/admin/buchungen', 'Buchungen'], ['#/admin/einstellungen', 'Einstellungen'],
  ]) {
    const a = el('a', { href, text: label });
    if (href === active) a.setAttribute('aria-current', 'page');
    nav.append(a);
  }
  return nav;
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
