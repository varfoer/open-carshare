import { api } from '../api.js';
import { parseEuroToCents, formatEuro } from '../money.js';
import { formatDateTime } from '../time.js';
import { dateTime, toEpoch } from '../form_time.js';
import { minutesToHHMM, hhmmToMinutes, parsePercentToPermille, tariffSummary } from '../price.js';
import { el, adminNav, messageBox, textField, checkField, primaryButton } from '../admin_ui.js';

const toEuroInput = (cents) => formatEuro(cents).replace(' €', '');

// Tariff section of one car: versions newest first, delete for future ones, new-version form.
async function tariffSection(car) {
  const box = el('div', { className: 'tariffs' });
  const err = messageBox('error');
  const list = el('div');
  const formHost = el('div');
  box.append(el('h2', { text: `Tarife ${car.name}` }), err.node, list, formHost);
  const tariffs = await api('GET', `/api/admin/cars/${car.id}/tariffs`);
  const now = Math.floor(Date.now() / 1000);
  const reload = async () => { box.replaceWith(await tariffSection(car)); };

  for (const t of tariffs) {
    const row = el('div', { className: 'booking' },
      el('p', { className: 'booking-car', text: `ab ${formatDateTime(t.valid_from)}: ${tariffSummary(t)}` }));
    if (t.valid_from > now) {
      const del = el('button', { type: 'button', className: 'danger', text: 'Löschen' });
      del.addEventListener('click', async () => {
        err.hide();
        try { await api('DELETE', `/api/admin/tariffs/${t.id}`); await reload(); } catch (x) { err.show(x.message); }
      });
      row.append(del);
    }
    list.append(row);
  }
  if (!tariffs.length) list.append(el('p', { className: 'placeholder', text: 'Kein Tarif.' }));

  // Prefilled with the newest version; empty for the first one.
  const p = tariffs[0];
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
  const form = el('form', { className: 'card' }, el('h2', { text: 'Neuer Tarif ab' }),
    el('label', {}, 'Gültig ab', when.date, when.time),
    day.node, night.node, km.node, ns.node, ne.node, unused.node, cap.node, ccap.node, late.node,
    primaryButton('Tarif anlegen'));
  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    err.hide();
    const cents = (f) => parseEuroToCents(f.input.value);
    const body = {
      valid_from: when.date.value ? toEpoch(when) : null,
      rate_day_cents_per_h: cents(day), rate_night_cents_per_h: cents(night), rate_km_cents: cents(km),
      night_start: hhmmToMinutes(ns.input.value), night_end: hhmmToMinutes(ne.input.value),
      unused_factor_permille: parsePercentToPermille(unused.input.value),
      time_cap_cents_per_day: cents(cap), combined_cap_cents_per_day: cents(ccap),
      late_fee_cents_per_started_h: cents(late),
    };
    if (Object.values(body).some((v) => v === null)) return err.show('Bitte alle Felder korrekt ausfüllen (Beträge wie 3 oder 3,50, Zeiten als HH:MM, Prozent 0–100).');
    try { await api('POST', `/api/admin/cars/${car.id}/tariffs`, body); await reload(); } catch (x) { err.show(x.message); }
  });
  formHost.append(form);
  return box;
}

export async function render(root) {
  const cars = await api('GET', '/api/admin/cars');
  const err = messageBox('error');
  const list = el('div');
  const formHost = el('div');
  root.append(adminNav('#/admin/autos'), el('h1', { text: 'Autos' }), err.node, list, formHost);

  const reload = () => { root.replaceChildren(); return render(root); };

  async function save(car, body) {
    err.hide();
    try {
      if (car) await api('PUT', `/api/admin/cars/${car.id}`, body);
      else await api('POST', '/api/admin/cars', body);
      await reload();
    } catch (x) { err.show(x.message); }
  }

  function showForm(car) {
    const name = textField('Name', car ? car.name : '', { required: true });
    const plate = textField('Kennzeichen', car ? car.plate : '', { required: true });
    const loc = textField('Standort', car ? car.location : '');
    const imei = textField('Tracker-IMEI', car ? car.tracker_imei : '', { inputmode: 'numeric', pattern: '\\d{15}' });
    const active = checkField('aktiv', car ? car.active : true);
    const cancel = el('button', { type: 'button', text: 'Abbrechen', className: 'btn' });
    cancel.addEventListener('click', () => formHost.replaceChildren());
    const form = el('form', { className: 'card' },
      el('h2', { text: car ? 'Auto bearbeiten' : 'Auto anlegen' }),
      name.node, plate.node, loc.node, imei.node, active.node, primaryButton('Speichern'), cancel);
    form.addEventListener('submit', (ev) => {
      ev.preventDefault();
      save(car, {
        name: name.input.value.trim(), plate: plate.input.value.trim(),
        location: loc.input.value.trim(), tracker_imei: imei.input.value.trim(), active: active.input.checked,
      });
    });
    formHost.replaceChildren(form);
    form.scrollIntoView();
  }

  for (const c of cars) {
    const edit = el('button', { type: 'button', text: 'Bearbeiten' });
    edit.addEventListener('click', () => showForm(c));
    const tar = el('button', { type: 'button', text: 'Tarife' });
    const tarHost = el('div');
    tar.addEventListener('click', async () => {
      err.hide();
      if (tarHost.hasChildNodes()) return tarHost.replaceChildren();
      try { tarHost.replaceChildren(await tariffSection(c)); } catch (x) { err.show(x.message); }
    });
    const actions = el('div', { className: 'dialog-actions' }, edit, tar);
    if (c.active) {
      const off = el('button', { type: 'button', className: 'danger', text: 'Deaktivieren' });
      off.addEventListener('click', () => save(c, { name: c.name, plate: c.plate, location: c.location, active: false }));
      actions.append(off);
    }
    list.append(el('div', { className: 'card booking' },
      el('p', { className: 'booking-time', text: `${c.name} (${c.plate})` }),
      el('p', { className: 'booking-car', text: `${c.location || 'ohne Standort'} · ${c.tracker_imei ? `Tracker ${c.tracker_imei}` : 'ohne Tracker'} · ${c.active ? 'aktiv' : 'inaktiv'}` }),
      actions, tarHost));
  }
  if (!cars.length) list.append(el('p', { className: 'placeholder', text: 'Keine Autos.' }));
  const add = el('button', { type: 'button', className: 'primary', text: 'Auto anlegen' });
  add.addEventListener('click', () => showForm(null));
  list.after(add);
}
