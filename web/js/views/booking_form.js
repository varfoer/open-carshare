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
    // Extend only: end may not move before the current end; start may move
    // earlier (never later) while the booking has not started yet.
    const startEditable = nowTs < booking.booked_start;
    s.date.disabled = !startEditable;
    s.time.disabled = !startEditable;
    hint.textContent = `Aktuell: ${formatDateTime(booking.booked_start)} bis ${formatDateTime(booking.booked_end)}. ` +
      (startEditable ? 'Möglich: früherer Start und/oder spätere Endzeit.' : 'Nur eine spätere Endzeit ist möglich.');
    // Clamp a date/time pair to a bound; dir -1: not after bound, +1: not before.
    const constrain = (ctl, ts, dir) => {
      const o = epochToBerlin(ts);
      const ds = dateStr(o), tm = timeStr(o);
      if (dir > 0) ctl.date.min = ds; else ctl.date.max = ds;
      const bad = (v) => (dir > 0 ? v < tm : v > tm);
      const refresh = () => {
        if (ctl.date.value && (dir > 0 ? ctl.date.value < ds : ctl.date.value > ds)) ctl.date.value = ds;
        const same = ctl.date.value === ds;
        for (const opt of ctl.time.options) opt.disabled = same && bad(opt.value);
        if (same && bad(ctl.time.value)) ctl.time.value = tm;
      };
      ctl.date.addEventListener('change', refresh);
      ctl.time.addEventListener('change', refresh);
      refresh();
    };
    constrain(e, booking.booked_end, 1);
    if (startEditable) constrain(s, booking.booked_start, -1);
  }

  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    err.hidden = true;
    if (!e.date.value || !s.date.value) return;
    const start = booking && s.date.disabled ? booking.booked_start : toEpoch(s);
    const end = toEpoch(e);
    const fail = (m) => { err.textContent = m; err.hidden = false; };
    if (end <= start) return fail('Das Ende muss nach dem Start liegen.');
    if (booking) {
      if (start > booking.booked_start || end < booking.booked_end) return fail('Die Buchung kann nur verlängert werden.');
      if (start === booking.booked_start && end === booking.booked_end) return fail('Keine Änderung.');
    }
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
