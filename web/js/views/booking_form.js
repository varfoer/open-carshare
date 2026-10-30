import { api } from '../api.js';
import { navigate } from '../app.js';
import { berlinToEpoch, epochToBerlin, formatDateTime } from '../time.js';

const p2 = (n) => String(n).padStart(2, '0');
const dateStr = (b) => `${b.y}-${p2(b.mo)}-${p2(b.d)}`;
const timeStr = (b) => `${p2(b.h)}:${p2(b.mi)}`;

function parseEpoch(v, fallback) {
  const n = Number(v);
  return Number.isFinite(n) && n > 0 ? n : fallback;
}

function field(label, ...controls) {
  const l = document.createElement('label');
  l.append(label, ...controls);
  return l;
}

// Date input + 15-minute time select. `extra` adds an off-grid time (existing bookings).
function dateTime(ts) {
  const b = epochToBerlin(ts);
  const date = document.createElement('input');
  date.type = 'date';
  date.required = true;
  date.value = dateStr(b);
  const time = document.createElement('select');
  const opts = [];
  for (let m = 0; m < 1440; m += 15) opts.push(`${p2(m / 60 | 0)}:${p2(m % 60)}`);
  if (!opts.includes(timeStr(b))) { opts.push(timeStr(b)); opts.sort(); }
  for (const t of opts) {
    const o = document.createElement('option');
    o.value = t;
    o.textContent = t;
    time.append(o);
  }
  time.value = timeStr(b);
  return { date, time };
}

function toEpoch({ date, time }) {
  const [y, mo, d] = date.value.split('-').map(Number);
  const [h, mi] = time.value.split(':').map(Number);
  return berlinToEpoch(y, mo, d, h, mi);
}

export async function render(root, params) {
  const [cars, existing] = await Promise.all([
    api('GET', '/api/cars'),
    params.id ? api('GET', '/api/bookings') : Promise.resolve(null),
  ]);
  let booking = null;
  if (params.id) {
    booking = existing.find((x) => String(x.id) === String(params.id));
    if (!booking) throw new Error('Buchung nicht gefunden.');
  }
  const carId = booking ? booking.car_id : parseEpoch(params.car, cars[0] && cars[0].id);
  const car = cars.find((c) => c.id === carId);
  const nowTs = Math.floor(Date.now() / 1000);
  const startTs = booking ? booking.booked_start : parseEpoch(params.start, Math.ceil(nowTs / 900) * 900);
  const endTs = booking ? booking.booked_end : parseEpoch(params.end, startTs + 7200);

  const form = document.createElement('form');
  form.className = 'card';
  const h1 = document.createElement('h1');
  h1.textContent = booking ? 'Buchung verlängern' : 'Buchen';
  const carLine = document.createElement('p');
  carLine.textContent = car ? `${car.name} (${car.plate})` : '';
  const s = dateTime(startTs);
  const e = dateTime(endTs);
  const err = document.createElement('p');
  err.className = 'error';
  err.setAttribute('role', 'alert');
  err.hidden = true;
  const hint = document.createElement('p');
  hint.className = 'hint';
  const submit = document.createElement('button');
  submit.type = 'submit';
  submit.className = 'primary';
  submit.textContent = booking ? 'Verlängern' : 'Buchen';
  const back = document.createElement('p');
  const a = document.createElement('a');
  a.href = booking ? '#/buchungen' : '#/kalender';
  a.textContent = 'Abbrechen';
  back.append(a);
  form.append(h1, carLine,
    field('Start', s.date, s.time), field('Ende', e.date, e.time), hint, err, submit, back);
  root.append(form);

  if (booking) {
    // Extend only: start is fixed, end may not move before the current end.
    s.date.disabled = true;
    s.time.disabled = true;
    hint.textContent = `Aktuell: ${formatDateTime(booking.booked_start)} bis ${formatDateTime(booking.booked_end)}. Nur eine spätere Endzeit ist möglich.`;
    const orig = epochToBerlin(booking.booked_end);
    e.date.min = dateStr(orig);
    const refresh = () => {
      const sameDay = e.date.value === dateStr(orig);
      for (const o of e.time.options) o.disabled = sameDay && o.value < timeStr(orig);
      if (sameDay && e.time.value < timeStr(orig)) e.time.value = timeStr(orig);
      if (e.date.value && e.date.value < dateStr(orig)) {
        e.date.value = dateStr(orig);
        refresh();
      }
    };
    e.date.addEventListener('change', refresh);
    e.time.addEventListener('change', refresh);
    refresh();
  }

  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    err.hidden = true;
    if (!e.date.value || !s.date.value) return;
    const start = booking ? booking.booked_start : toEpoch(s);
    const end = toEpoch(e);
    const fail = (m) => { err.textContent = m; err.hidden = false; };
    if (end <= start) return fail('Das Ende muss nach dem Start liegen.');
    if (booking && end < booking.booked_end) return fail('Die Buchung kann nur verlängert werden.');
    submit.disabled = true;
    try {
      if (booking) await api('PUT', `/api/bookings/${booking.id}`, { start, end });
      else await api('POST', '/api/bookings', { car_id: carId, start, end });
      navigate('#/buchungen');
    } catch (x) {
      fail(x.message);
      submit.disabled = false;
    }
  });
}
