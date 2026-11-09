import { api } from '../api.js';
import { parseEuroToCents, formatEuro } from '../money.js';
import { formatDateTime } from '../time.js';
import { dateTime, toEpoch } from '../form_time.js';
import { minutesToHHMM, hhmmToMinutes, parsePercentToPermille, tariffSummary } from '../price.js';
import { el, adminNav, messageBox, textField, primaryButton } from '../admin_ui.js';

const toEuroInput = (cents) => formatEuro(cents).replace(' €', '');

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
  const cap = euro('Deckel Zeit (€/Tag, 0 = keiner)', 'time_cap_cents_per_day');
  const ccap = euro('Deckel gesamt (€/Tag, 0 = keiner)', 'combined_cap_cents_per_day');
  const late = euro('Verspätung (€ je angefangene Stunde)', 'late_fee_cents_per_started_h');
  const form = el('form', {}, el('h3', { text: 'Neue Version ab' }),
    el('label', {}, 'Gültig ab', when.date, when.time),
    day.node, night.node, km.node, ns.node, ne.node, unused.node, cap.node, ccap.node, late.node,
    primaryButton('Version anlegen'));
  form.addEventListener('submit', (ev) => {
    ev.preventDefault();
    const cents = (f) => parseEuroToCents(f.input.value);
    const body = {
      valid_from: when.date.value ? toEpoch(when) : null,
      rate_day_cents_per_h: cents(day), rate_night_cents_per_h: cents(night), rate_km_cents: cents(km),
      night_start: hhmmToMinutes(ns.input.value), night_end: hhmmToMinutes(ne.input.value),
      unused_factor_permille: parsePercentToPermille(unused.input.value),
      time_cap_cents_per_day: cents(cap), combined_cap_cents_per_day: cents(ccap),
      late_fee_cents_per_started_h: cents(late),
    };
    if (Object.values(body).some((v) => v === null)) {
      err.show('Bitte alle Felder korrekt ausfüllen (Beträge wie 3 oder 3,50, Zeiten als HH:MM, Prozent 0–100).');
      err.node.scrollIntoView();
      return;
    }
    run('POST', `/api/admin/tariffs/${t.id}/versions`, body);
  });
  return form;
}

export async function render(root) {
  const tariffs = await api('GET', '/api/admin/tariffs');
  const err = messageBox('error');
  const list = el('div');
  root.append(adminNav('#/admin/tarife'), el('h1', { text: 'Tarife' }), err.node, list);

  const reload = () => { root.replaceChildren(); return render(root); };
  // Every change: call the server, reload on success, show the server's message otherwise.
  const run = async (method, path, body) => {
    err.hide();
    try { await api(method, path, body); await reload(); } catch (x) { err.show(x.message); err.node.scrollIntoView(); }
  };

  const now = Math.floor(Date.now() / 1000);
  // Fallback prefill for tariffs without versions: newest version of the first tariff.
  const fallback = tariffs.length ? tariffs[0].versions[0] : undefined;

  for (const t of tariffs) {
    const name = textField('Name', t.name, { required: true });
    const rename = el('form', { className: 'dialog-actions' }, name.node, primaryButton('Umbenennen'));
    rename.addEventListener('submit', (ev) => {
      ev.preventDefault();
      run('PUT', `/api/admin/tariffs/${t.id}`, { name: name.input.value.trim() });
    });
    const del = el('button', { type: 'button', className: 'danger', text: 'Löschen' });
    del.addEventListener('click', () => run('DELETE', `/api/admin/tariffs/${t.id}`));

    const versions = el('div');
    for (const v of t.versions) {
      const row = el('div', { className: 'booking' },
        el('p', { className: 'booking-car', text: `ab ${formatDateTime(v.valid_from)}: ${tariffSummary(v)}` }));
      if (v.valid_from > now) {
        const vdel = el('button', { type: 'button', className: 'danger', text: 'Löschen' });
        vdel.addEventListener('click', () => run('DELETE', `/api/admin/tariff-versions/${v.id}`));
        row.append(vdel);
      }
      versions.append(row);
    }
    if (!t.versions.length) versions.append(el('p', { className: 'placeholder', text: 'Keine Version.' }));

    list.append(el('div', { className: 'card' },
      el('h2', { text: t.name }), rename, del, versions,
      versionForm(t, t.versions[0] || fallback, now, run, err)));
  }
  if (!tariffs.length) list.append(el('p', { className: 'placeholder', text: 'Keine Tarife.' }));

  const newName = textField('Name', '', { required: true });
  const create = el('form', { className: 'card' }, el('h2', { text: 'Neuer Tarif' }), newName.node, primaryButton('Tarif anlegen'));
  create.addEventListener('submit', (ev) => {
    ev.preventDefault();
    run('POST', '/api/admin/tariffs', { name: newName.input.value.trim() });
  });
  list.after(create);
}
