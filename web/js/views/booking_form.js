import { api } from '../api.js';
import { navigate, currentUser } from '../app.js';
import { epochToBerlin, formatDateTime } from '../time.js';
import { dateStr, timeStr, field, dateTime, toEpoch } from '../form_time.js';

function parseEpoch(v, fallback) {
  const n = Number(v);
  return Number.isFinite(n) && n > 0 ? n : fallback;
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
  // Club trip checkbox: only for permitted users (or if the booking already is one).
  const me = currentUser();
  const showClub = (me && me.may_club_trip) || (booking && booking.club_trip);
  const club = document.createElement('input');
  club.type = 'checkbox';
  club.checked = !!(booking && booking.club_trip);
  const clubLabel = document.createElement('label');
  clubLabel.className = 'check';
  clubLabel.append(club, ' Vereinsfahrt (wird nicht berechnet)');
  form.append(h1, carLine,
    field('Start', s.date, s.time), field('Ende', e.date, e.time),
    ...(showClub ? [clubLabel] : []), hint, err, submit, back);
  root.append(form);

  if (booking) {
    // Extend only: end may not move before the current end; start may move
    // earlier (never later) while the booking has not started yet.
    const startEditable = nowTs < booking.booked_start;
    club.disabled = !startEditable; // the flag is fixed once the booking has started
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
      if (start === booking.booked_start && end === booking.booked_end && club.checked === !!booking.club_trip) return fail('Keine Änderung.');
    }
    submit.disabled = true;
    try {
      if (booking) {
        const body = { start, end };
        if (showClub && !club.disabled) body.club_trip = club.checked;
        await api('PUT', `/api/bookings/${booking.id}`, body);
      } else {
        const body = { car_id: carId, start, end };
        if (showClub) body.club_trip = club.checked;
        await api('POST', '/api/bookings', body);
      }
      navigate('#/buchungen');
    } catch (x) {
      fail(x.message);
      submit.disabled = false;
    }
  });
}
