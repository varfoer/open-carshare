import { api } from '../api.js';
import { el, adminNav, messageBox, selectField, primaryButton } from '../admin_ui.js';
import { epochToBerlin, formatDateTime, monthRange } from '../time.js';
import { field, dateTime, toEpoch, p2 } from '../form_time.js';

const STATES = { reserved: 'reserviert', active: 'aktiv', done: 'beendet', cancelled: 'storniert' };

// Module state survives re-render (reload after edit keeps the filter).
const filter = { car: '', month: '' };

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
  root.append(adminNav('#/admin/buchungen'), el('h1', { text: 'Buchungen' }), bar, err.node, list, formHost);

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
      if (b.state === 'reserved' || b.state === 'active') {
        const edit = el('button', { type: 'button', text: 'Bearbeiten' });
        edit.addEventListener('click', () => showForm(b));
        actions.append(edit);
      }
      if (b.state === 'reserved') {
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
      list.append(el('div', { className: 'card booking' },
        el('p', { className: 'booking-time', text: `${formatDateTime(b.booked_start)} – ${formatDateTime(b.booked_end)}` }),
        el('p', { className: 'booking-car',
          text: `${carName.get(b.car_id) || b.car_id} · ${b.user_name} (${b.user_email}) · ${STATES[b.state] || b.state}` }),
        actions));
    }
  }

  function showForm(b) {
    const carF = selectField('Auto', cars.map((c) => [c.id, c.active ? c.name : `${c.name} (inaktiv)`]), b.car_id);
    const userF = selectField('Nutzer', users.map((u) => [u.id, `${u.name} (${u.email})`]), b.user_id);
    const s = dateTime(b.booked_start);
    const e = dateTime(b.booked_end);
    const fErr = messageBox('error');
    const cancel = el('button', { type: 'button', text: 'Abbrechen', className: 'btn' });
    cancel.addEventListener('click', () => formHost.replaceChildren());
    const form = el('form', { className: 'card' },
      el('h2', { text: 'Buchung bearbeiten' }), carF.node, userF.node,
      field('Start', s.date, s.time), field('Ende', e.date, e.time), fErr.node,
      primaryButton('Speichern'), cancel);
    form.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      fErr.hide();
      if (!s.date.value || !e.date.value) return;
      const start = toEpoch(s), end = toEpoch(e);
      if (end <= start) return fErr.show('Das Ende muss nach dem Start liegen.');
      try {
        await api('PUT', `/api/admin/bookings/${b.id}`,
          { car_id: Number(carF.input.value), user_id: Number(userF.input.value), start, end });
        formHost.replaceChildren();
        await load();
      } catch (x) { fErr.show(x.message); }
    });
    formHost.replaceChildren(form);
    form.scrollIntoView();
  }

  carSel.input.addEventListener('change', load);
  month.addEventListener('change', load);
  await load();
}
