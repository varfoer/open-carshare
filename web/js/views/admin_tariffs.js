import { api } from '../api.js';
import { parseEuroToCents, formatEuro } from '../money.js';
import { epochToBerlin } from '../time.js';
import { dateTime, toEpoch, p2 } from '../form_time.js';
import { minutesToHHMM, hhmmToMinutes, parsePercentToPermille } from '../price.js';
import { el, adminPage, pageHead, badge, messageBox, textField, primaryButton } from '../admin_ui.js';
import { icon } from '../icons.js';

const toEuroInput = (cents) => formatEuro(cents).replace(' €', '');
// Berlin date of a version start; the time only when it is not midnight (several versions may start on one day).
const validFrom = (ts) => {
  const b = epochToBerlin(ts);
  const date = `${p2(b.d)}.${p2(b.mo)}.${b.y}`;
  return b.h || b.mi ? `${date}, ${p2(b.h)}:${p2(b.mi)}` : date;
};
const perH = (cents) => `${formatEuro(cents)}/h`;
const cap = (cents) => (cents ? formatEuro(cents) : 'keiner');

// Selected tariff and open forms survive a reload after a change.
const ui = { selected: null, create: false, version: false, rename: false };

// Version in force now (largest valid_from <= now), else the earliest upcoming one.
function currentVersion(t, now) {
  const past = t.versions.filter((v) => v.valid_from <= now);
  if (past.length) return past.reduce((a, b) => (b.valid_from > a.valid_from ? b : a));
  return t.versions.length ? t.versions.reduce((a, b) => (b.valid_from < a.valid_from ? b : a)) : null;
}

// Form for a new version of tariff t, prefilled from version p (or empty when p is undefined).
function versionForm(t, p, now, run, err) {
  // Existing versions: default to the next 15-minute slot after now (server rejects valid_from in the past).
  const when = dateTime(p ? Math.ceil((now + 900) / 900) * 900 : now);
  const euro = (label, key) => textField(label, p ? toEuroInput(p[key]) : '', { inputMode: 'decimal', required: true });
  const day = euro('Tag (€/h)', 'rate_day_cents_per_h');
  const night = euro('Nacht (€/h)', 'rate_night_cents_per_h');
  const km = euro('Kilometer (€/km)', 'rate_km_cents');
  const ns = textField('Nacht von (HH:MM)', p ? minutesToHHMM(p.night_start) : '', { inputMode: 'numeric', required: true });
  const ne = textField('Nacht bis (HH:MM)', p ? minutesToHHMM(p.night_end) : '', { inputMode: 'numeric', required: true });
  const unused = textField('Nicht genutzte Zeit (%)', p ? String(p.unused_factor_permille / 10) : '', { inputMode: 'decimal', required: true });
  const tcap = euro('Deckel Zeit (€/Tag, 0 = keiner)', 'time_cap_cents_per_day');
  const ccap = euro('Deckel gesamt (€/Tag, 0 = keiner)', 'combined_cap_cents_per_day');
  const late = euro('Verspätung (€ je angefangene Stunde)', 'late_fee_cents_per_started_h');
  const form = el('form', { className: 'card' }, el('h3', { text: 'Neue Version' }),
    el('label', {}, 'Gültig ab', when.date, when.time),
    day.node, night.node, km.node, ns.node, ne.node, unused.node, tcap.node, ccap.node, late.node,
    primaryButton('Version anlegen'));
  form.addEventListener('submit', (ev) => {
    ev.preventDefault();
    const cents = (f) => parseEuroToCents(f.input.value);
    const body = {
      valid_from: when.date.value ? toEpoch(when) : null,
      rate_day_cents_per_h: cents(day), rate_night_cents_per_h: cents(night), rate_km_cents: cents(km),
      night_start: hhmmToMinutes(ns.input.value), night_end: hhmmToMinutes(ne.input.value),
      unused_factor_permille: parsePercentToPermille(unused.input.value),
      time_cap_cents_per_day: cents(tcap), combined_cap_cents_per_day: cents(ccap),
      late_fee_cents_per_started_h: cents(late),
    };
    if (Object.values(body).some((v) => v === null)) {
      err.show('Bitte alle Felder korrekt ausfüllen (Beträge wie 3 oder 3,50, Zeiten als HH:MM, Prozent 0–100).');
      err.node.scrollIntoView();
      return;
    }
    ui.version = false;
    run('POST', `/api/admin/tariffs/${t.id}/versions`, body);
  });
  return form;
}

export async function render(root) {
  const [tariffs, users] = await Promise.all([api('GET', '/api/admin/tariffs'), api('GET', '/api/admin/users')]);
  const err = messageBox('error');
  const add = el('button', { type: 'button', className: 'primary' }, icon('plus'), el('span', { text: 'Tarif anlegen' }));
  const main = adminPage(root, '#/admin/tarife');
  main.append(pageHead('Tarife', 'Jeder Nutzer hat genau einen Tarif. Preise ändern sich über neue Versionen.', add), err.node);

  const reload = () => { root.replaceChildren(); return render(root); };
  // Every change: call the server, reload on success, show the server's message otherwise.
  const run = async (method, path, body) => {
    err.hide();
    try { await api(method, path, body); await reload(); } catch (x) { err.show(x.message); err.node.scrollIntoView(); }
  };
  const now = Math.floor(Date.now() / 1000);

  const newName = textField('Name', '', { required: true });
  const create = el('form', { className: 'card' }, el('h2', { text: 'Neuer Tarif' }), newName.node, primaryButton('Tarif anlegen'));
  create.hidden = !ui.create && tariffs.length > 0;
  create.addEventListener('submit', (ev) => {
    ev.preventDefault();
    ui.create = false;
    run('POST', '/api/admin/tariffs', { name: newName.input.value.trim() });
  });
  add.addEventListener('click', () => { ui.create = create.hidden; create.hidden = !create.hidden; if (!create.hidden) newName.input.focus(); });
  main.append(create);
  if (!tariffs.length) { main.append(el('p', { className: 'placeholder', text: 'Keine Tarife.' })); return; }

  const selected = tariffs.find((t) => t.id === ui.selected) || tariffs[0];
  ui.selected = selected.id;
  const layout = el('div', { className: 'tariff-layout' });

  // Left: one card per tariff with user count and the current rates.
  const list = el('nav', { className: 'tariff-list' });
  list.setAttribute('aria-label', 'Tarife');
  for (const t of tariffs) {
    const cur = currentVersion(t, now);
    const n = users.filter((u) => u.tariff_id === t.id).length;
    const sum = !cur ? 'Keine Version'
      : `${perH(cur.rate_day_cents_per_h)} · ${formatEuro(cur.rate_km_cents)}/km · ${cur.valid_from <= now ? 'seit' : 'ab'} ${validFrom(cur.valid_from)}`;
    const a = el('a', { href: '#/admin/tarife' },
      el('div', { className: 'row' }, el('span', { text: t.name }), badge(`${n} Nutzer`)),
      el('span', { className: 'sum', text: sum }));
    if (t === selected) a.setAttribute('aria-current', 'true');
    // Switch the detail from the loaded data: no refetch, nothing to race with a route change.
    a.addEventListener('click', (ev) => {
      ev.preventDefault();
      ui.selected = t.id; ui.version = false; ui.rename = false;
      for (const o of list.children) o.removeAttribute('aria-current');
      a.setAttribute('aria-current', 'true');
      err.hide();
      layout.replaceChildren(list, detailFor(t));
    });
    list.append(a);
  }

  // Right: the selected tariff.
  const detailFor = (t) => {
    const cur = currentVersion(t, now);
    const renameBtn = el('button', { type: 'button', className: 'btn', text: 'Umbenennen' });
    const del = el('button', { type: 'button', className: 'danger', text: 'Löschen' });
    del.addEventListener('click', () => {
      if (!window.confirm(`Tarif „${t.name}“ mit allen Versionen löschen?`)) return;
      run('DELETE', `/api/admin/tariffs/${t.id}`);
    });
    const name = textField('Name', t.name, { required: true });
    const rename = el('form', { className: 'dialog-actions' }, name.node, primaryButton('Umbenennen'));
    rename.hidden = !ui.rename;
    rename.addEventListener('submit', (ev) => {
      ev.preventDefault();
      ui.rename = false;
      run('PUT', `/api/admin/tariffs/${t.id}`, { name: name.input.value.trim() });
    });
    renameBtn.addEventListener('click', () => { rename.hidden = !rename.hidden; ui.rename = !rename.hidden; if (!rename.hidden) name.input.focus(); });

    const tiles = el('div', { className: 'rate-tiles' });
    if (cur) {
      const tile = (k, v) => el('div', { className: 'tile' }, el('span', { className: 'k', text: k }), el('span', { className: 'v', text: v }));
      tiles.append(
        tile('Tag', perH(cur.rate_day_cents_per_h)),
        tile(`Nacht ${minutesToHHMM(cur.night_start)} – ${minutesToHHMM(cur.night_end)}`, perH(cur.rate_night_cents_per_h)),
        tile('Kilometer', `${formatEuro(cur.rate_km_cents)}/km`),
        tile('Deckel pro Tag', cap(cur.time_cap_cents_per_day)));
    }

    const newVersion = el('button', { type: 'button', className: 'soft', text: 'Neue Version' });
    const vform = versionForm(t, t.versions[0] || (tariffs[0] && tariffs[0].versions[0]), now, run, err);
    vform.hidden = !ui.version;
    newVersion.addEventListener('click', () => { vform.hidden = !vform.hidden; ui.version = !vform.hidden; });

    const table = el('table', { className: 'data' });
    const head = el('tr');
    for (const h of ['Gültig ab', 'Tag', 'Nacht', 'km', 'Ungenutzt', 'Deckel Zeit / gesamt', 'Verspätung', '']) head.append(el('th', { text: h }));
    table.append(el('thead', {}, head));
    const body = el('tbody');
    for (const v of t.versions) {
      const when = el('td', { className: 'num', text: validFrom(v.valid_from) });
      if (v === cur && v.valid_from <= now) when.append(' ', badge('Aktuell', 'ok'));
      else if (v.valid_from > now) when.append(' ', badge('Geplant', 'warn'));
      const actions = el('td', { className: 'actions' });
      if (v.valid_from > now) {
        const vdel = el('button', { type: 'button', className: 'danger', text: 'Löschen' });
        vdel.addEventListener('click', () => {
          if (!window.confirm(`Version ab ${validFrom(v.valid_from)} löschen?`)) return;
          run('DELETE', `/api/admin/tariff-versions/${v.id}`);
        });
        actions.append(vdel);
      }
      body.append(el('tr', { className: v.valid_from <= now && v !== cur ? 'inactive' : '' }, when,
        el('td', { className: 'num', text: perH(v.rate_day_cents_per_h) }),
        el('td', { className: 'num', text: `${perH(v.rate_night_cents_per_h)} (${minutesToHHMM(v.night_start)}–${minutesToHHMM(v.night_end)})` }),
        el('td', { className: 'num', text: formatEuro(v.rate_km_cents) }),
        el('td', { className: 'num', text: `${v.unused_factor_permille / 10} %` }),
        el('td', { className: 'num', text: `${cap(v.time_cap_cents_per_day)} / ${cap(v.combined_cap_cents_per_day)}` }),
        el('td', { className: 'num', text: perH(v.late_fee_cents_per_started_h) }),
        actions));
    }
    table.append(body);
    const versions = t.versions.length
      ? el('div', { className: 'table-wrap' }, table)
      : el('p', { className: 'placeholder', text: 'Keine Version.' });

    return el('section', { className: 'card tariff-detail' },
      el('div', { className: 'page-head' }, el('h2', { text: t.name }), el('div', { className: 'actions' }, renameBtn, del)),
      rename, tiles,
      el('div', { className: 'sec-head' }, el('h3', { text: 'Versionen' }), newVersion),
      versions, vform);
  };

  layout.append(list, detailFor(selected));
  main.append(layout);
}
