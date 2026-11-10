import { api } from '../api.js';
import { el, adminPage, pageHead, messageBox, selectField, checkField, primaryButton } from '../admin_ui.js';
import { epochToBerlin, formatDateTime, monthRange } from '../time.js';
import { priceSummary } from '../price.js';
import { field, dateTime, toEpoch, p2 } from '../form_time.js';

const STATES = { reserved: 'reserviert', active: 'aktiv', done: 'beendet', cancelled: 'storniert' };

// Module state survives re-render (reload after edit keeps the filter).
const filter = { car: '', month: '' };

function kmText(b) {
  if (b.km_source === 'none') return '';
  return `${b.km_start} – ${b.km_end} km (${b.km_end - b.km_start} km)`;
}

const PHASES = { start: 'bei Start', end: 'bei Rückgabe' };

// Date + time pair for an optional timestamp (0 = unset); "leeren" empties the date.
function optionalDateTime(ts, fallback) {
  const f = dateTime(ts || fallback);
  f.date.required = false;
  if (!ts) f.date.value = '';
  const clear = el('button', { type: 'button', text: 'leeren' });
  clear.addEventListener('click', () => { f.date.value = ''; });
  const initial = { date: f.date.value, time: f.time.value };
  // Untouched fields keep the stored value (to the second); toEpoch only has minutes.
  f.value = () => {
    if (!f.date.value) return null;
    if (ts && f.date.value === initial.date && f.time.value === initial.time) return ts;
    return toEpoch(f);
  };
  f.clear = clear;
  return f;
}

function currentMonth() {
  const b = epochToBerlin(Math.floor(Date.now() / 1000));
  return `${b.y}-${p2(b.mo)}`;
}

export async function render(root) {
  if (!filter.month) filter.month = currentMonth();
  const [cars, users] = await Promise.all([api('GET', '/api/admin/cars'), api('GET', '/api/admin/users')]);

  const carSel = selectField('Auto', [['', 'Alle'], ...cars.map((c) => [c.id, c.name])], filter.car);
  const month = el('input', { type: 'month', value: filter.month });
  const monthLabel = el('label', {}, 'Monat', month);
  const err = messageBox('error');
  const list = el('div');
  const formHost = el('div');
  const bar = el('div', { className: 'filter-bar' }, carSel.node, monthLabel);
  adminPage(root, '#/admin/buchungen').append(pageHead('Buchungen'), bar, err.node, list, formHost);

  async function load() {
    err.hide();
    filter.car = carSel.input.value;
    filter.month = month.value;
    const q = new URLSearchParams();
    if (filter.car) q.set('car', filter.car);
    if (/^\d{4}-\d{2}$/.test(filter.month)) {
      const [y, mo] = filter.month.split('-').map(Number);
      const r = monthRange(y, mo);
      q.set('from', r.from);
      q.set('to', r.to);
    }
    let rows;
    try {
      rows = await api('GET', `/api/admin/bookings?${q}`);
    } catch (x) { return err.show(x.message); }
    list.replaceChildren();
    if (!rows.length) list.append(el('p', { className: 'placeholder', text: 'Keine Buchungen.' }));
    const carName = new Map(cars.map((c) => [c.id, c.name]));
    for (const b of rows) {
      const actions = el('div', { className: 'dialog-actions' });
      if (b.state !== 'cancelled' && !b.invoice_id) {
        const edit = el('button', { type: 'button', text: 'Bearbeiten' });
        edit.addEventListener('click', () => showForm(b));
        actions.append(edit);
      }
      if (b.state === 'reserved' && !b.invoice_id) {
        const del = el('button', { type: 'button', className: 'danger', text: 'Stornieren' });
        del.addEventListener('click', async () => {
          if (!window.confirm(`Buchung von ${b.user_name} (${formatDateTime(b.booked_start)}) stornieren?`)) return;
          try {
            await api('DELETE', `/api/admin/bookings/${b.id}`);
            await load();
          } catch (x) { err.show(x.message); }
        });
        actions.append(del);
      }
      // Every row: reports outlive cleared actual times.
      const dmg = el('button', { type: 'button', text: 'Schäden' });
      dmg.addEventListener('click', () => showDamage(b));
      actions.append(dmg);
      const trip = [];
      if (b.actual_start) {
        trip.push(el('p', { text: `Gefahren: ${formatDateTime(b.actual_start)} – ${b.actual_end ? formatDateTime(b.actual_end) : 'läuft'}` }));
        if (b.km_pending) trip.push(el('p', {}, el('span', { className: 'badge', text: 'km ausstehend' })));
        else if (kmText(b)) trip.push(el('p', { text: `${kmText(b)}${b.km_source === 'manual' ? ' · manuell' : ''}${b.km_source === 'telematics' ? ' · Tracker' : ''}` }));
      }
      list.append(el('div', { className: 'card booking' },
        el('p', { className: 'booking-time', text: `${formatDateTime(b.booked_start)} – ${formatDateTime(b.booked_end)}` }),
        el('p', { className: 'booking-car',
          text: `${carName.get(b.car_id) || b.car_id} · ${b.user_name} (${b.user_email}) · ${STATES[b.state] || b.state}` },
          ...(b.club_trip ? [' ', el('span', { className: 'badge', text: 'Vereinsfahrt' })] : []),
          ...(b.invoice_id ? [' ', el('span', { className: 'badge', text: `Rechnung ${b.invoice_id}` })] : [])),
        ...trip, ...(b.price && (b.state === 'done' || b.state === 'cancelled') ? [el('p', { text: `Preis: ${priceSummary(b.price)}` })] : []), actions));
    }
  }

  function showForm(b) {
    const carF = selectField('Auto', cars.map((c) => [c.id, c.active ? c.name : `${c.name} (inaktiv)`]), b.car_id);
    const userF = selectField('Nutzer', users.map((u) => [u.id, `${u.name} (${u.email})`]), b.user_id);
    const s = dateTime(b.booked_start);
    const e = dateTime(b.booked_end);
    const as = optionalDateTime(b.actual_start, b.booked_start);
    const ae = optionalDateTime(b.actual_end, b.booked_end);
    const kmS0 = b.km_source === 'none' ? '' : String(b.km_start);
    const kmE0 = b.km_source === 'none' ? '' : String(b.km_end);
    const kmS = el('input', { type: 'number', min: 0, max: 10000000, step: 1, value: kmS0 });
    const kmE = el('input', { type: 'number', min: 0, max: 10000000, step: 1, value: kmE0 });
    const club = checkField('Vereinsfahrt (wird nicht berechnet)', !!b.club_trip);
    const fErr = messageBox('error');
    const cancel = el('button', { type: 'button', text: 'Abbrechen', className: 'btn' });
    cancel.addEventListener('click', () => formHost.replaceChildren());
    const form = el('form', { className: 'card' },
      el('h2', { text: 'Buchung bearbeiten' }), carF.node, userF.node,
      field('Start', s.date, s.time), field('Ende', e.date, e.time),
      field('Tatsächlicher Beginn', as.date, as.time, as.clear),
      field('Tatsächliches Ende', ae.date, ae.time, ae.clear),
      field('km Start', kmS), field('km Ende', kmE), club.node, fErr.node,
      primaryButton('Speichern'), cancel);
    form.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      fErr.hide();
      if (!s.date.value || !e.date.value) return;
      const start = toEpoch(s), end = toEpoch(e);
      if (end <= start) return fErr.show('Das Ende muss nach dem Start liegen.');
      const body = { car_id: Number(carF.input.value), user_id: Number(userF.input.value), start, end,
        club_trip: club.input.checked };
      // Any present actual_* key makes the server re-derive the state: send both, only on change.
      const nas = as.value(), nae = ae.value();
      if (nas !== (b.actual_start || null) || nae !== (b.actual_end || null)) {
        body.actual_start = nas;
        body.actual_end = nae;
      }
      // Sending km turns the source into manual: only when the admin changed a value.
      if (kmS.value !== '' || kmE.value !== '') {
        if (kmS.value === '' || kmE.value === '') return fErr.show('Bitte km Start und km Ende angeben.');
        if (kmS.value !== kmS0 || kmE.value !== kmE0) {
          body.km_start = Number(kmS.value);
          body.km_end = Number(kmE.value);
        }
      }
      try {
        await api('PUT', `/api/admin/bookings/${b.id}`, body);
        formHost.replaceChildren();
        await load();
      } catch (x) { fErr.show(x.message); }
    });
    formHost.replaceChildren(form);
    form.scrollIntoView();
  }

  async function showDamage(b) {
    const box = el('div', { className: 'card' }, el('h2', { text: 'Schäden' }));
    const close = el('button', { type: 'button', className: 'btn', text: 'Schließen' });
    close.addEventListener('click', () => formHost.replaceChildren());
    formHost.replaceChildren(box);
    let reports;
    try {
      reports = await api('GET', `/api/bookings/${b.id}/damage`);
    } catch (x) { box.append(el('p', { className: 'error', text: x.message })); }
    if (reports && !reports.length) box.append(el('p', { className: 'placeholder', text: 'Keine Schadensmeldungen.' }));
    for (const r of reports || []) {
      const imgs = el('div', { className: 'damage-photos' });
      for (let k = 1; k <= r.n_photos; k++) {
        const url = `/api/damage/${r.id}/photos/${k}`;
        imgs.append(el('a', { href: url, target: '_blank', rel: 'noopener' },
          el('img', { src: url, loading: 'lazy', alt: `Foto ${k}` })));
      }
      box.append(el('div', { className: 'damage-report' },
        el('p', { className: 'booking-time', text: `${PHASES[r.phase] || r.phase} · ${formatDateTime(r.created_at)}` }),
        el('p', { text: r.text }), imgs));
    }
    box.append(close);
    box.scrollIntoView();
  }

  carSel.input.addEventListener('change', load);
  month.addEventListener('change', load);
  await load();
}
