import { api } from '../api.js';
import { navigate } from '../app.js';
import { formatDateTime, formatTime, sameBerlinDay } from '../time.js';
import { formatEuro } from '../money.js';
import { tripActions, pinView } from '../trip.js';
import { liveLine } from '../price.js';
import { apiUpload, resizeToJpeg } from '../photo.js';
import { damageDialog } from './damage_dialog.js';
import { pinDialog, pinDisplay } from './pin_dialog.js';
import { bookingCard } from '../booking_card.js';
import { el } from '../admin_ui.js';

// Description of a failed start report per booking id, offered again by "Schaden melden".
const retryText = new Map();

async function fileReport(b, phase, damage) {
  const report = await api('POST', `/api/bookings/${b.id}/damage`, { phase, text: damage.text });
  for (const f of damage.files) {
    await apiUpload(`/api/damage/${report.id}/photos`, await resizeToJpeg(f));
  }
}

// Start or return: confirm damage state, call the trip endpoint, then file the report.
// Server-side trip state stays as is when a later step fails; the caller reloads the list.
async function runTrip(b, action, title, phase, hasBox = false) {
  if (action === 'return' && hasBox && !await pinDialog(b.id)) return false;
  const damage = await damageDialog(title, phase === 'start' ? { text: retryText.get(b.id) } : {});
  if (damage === null) return false;
  if (damage !== 'none' && phase === 'start') retryText.set(b.id, damage.text);
  await api('POST', `/api/bookings/${b.id}/${action}`);
  if (damage === 'none') { retryText.delete(b.id); return true; }
  try {
    await fileReport(b, phase, damage);
  } catch (x) {
    const done = phase === 'start' ? 'Fahrt gestartet' : 'Fahrzeug zurückgegeben';
    throw new Error(`${done}, aber die Schadensmeldung ist fehlgeschlagen: ${x.message.replace(/\.$/, '')}. Bitte erneut melden.`);
  }
  retryText.delete(b.id);
  return true;
}

// Damage found during an active trip (or a failed start report): files a start-phase report.
async function runReport(b) {
  const damage = await damageDialog('Schaden melden', { reportOnly: true, text: retryText.get(b.id) });
  if (damage === null) return false;
  try {
    await fileReport(b, 'start', damage);
  } catch (x) {
    retryText.set(b.id, damage.text);
    throw x;
  }
  retryText.delete(b.id);
  return true;
}

function confirmDialog(text) {
  return new Promise((resolve) => {
    const dlg = el('dialog', { className: 'confirm' });
    const no = el('button', { type: 'button', text: 'Zurück' });
    const yes = el('button', { type: 'button', className: 'danger', text: 'Stornieren' });
    dlg.append(el('p', { text }), el('div', { className: 'dialog-actions' }, no, yes));
    let result = false;
    no.addEventListener('click', () => dlg.close());
    yes.addEventListener('click', () => { result = true; dlg.close(); });
    dlg.addEventListener('close', () => { dlg.remove(); resolve(result); });
    document.body.append(dlg);
    dlg.showModal();
  });
}

// Second line under the time: today marker, trip start, or the car for multi-car clubs.
function subline(b, now, carName, carCount) {
  const parts = [];
  if (b.state === 'active') {
    // Full date when the trip did not start today (e.g. started last night).
    const since = sameBerlinDay(b.actual_start, now) ? formatTime(b.actual_start) : formatDateTime(b.actual_start);
    parts.push(`Unterwegs seit ${since}`);
  } else if (sameBerlinDay(b.booked_start, now)) parts.push('Heute');
  if (carCount > 1 && carName) parts.push(carName);
  return parts.join(' · ');
}

// GET /pin of an active booking: { p } with the answer, { none: true } for 404 (no key box),
// { error } with the message for anything else. Independent of the car list, which omits inactive cars.
async function probePin(b) {
  try {
    return { p: await api('GET', `/api/bookings/${b.id}/pin`) };
  } catch (x) {
    return x.status === 404 ? { none: true } : { error: x.message };
  }
}

// PIN line of an active booking with a key box. A failed fetch shows its message in the card only.
function pinLine(b, now, err, probe) {
  const box = el('div', { className: 'pin-box' });
  const fail = el('p', { className: 'error', role: 'alert' });
  fail.hidden = true;
  const call = async (method, body) => {
    try {
      return await api(method, `/api/bookings/${b.id}/pin`, body);
    } catch (x) {
      fail.textContent = x.message;
      fail.hidden = false;
      return null;
    }
  };
  const act = (label, className, renew) => {
    const btn = el('button', { type: 'button', className, text: label });
    btn.addEventListener('click', async () => {
      err.hidden = true;
      btn.disabled = true;
      if (await call('POST', { renew })) navigate('#/buchungen');
      else btn.disabled = false;
    });
    return btn;
  };
  const p = probe.p;
  if (probe.error) {
    fail.textContent = probe.error;
    fail.hidden = false;
  }
  if (p) {
    const v = pinView(p, now);
    if (v.kind === 'show') {
      const until = sameBerlinDay(v.until, now) ? formatTime(v.until) : formatDateTime(v.until);
      box.append(pinDisplay(v.pin),
        el('p', { className: 'booking-car', text: `Gültig bis ${until}` }),
        act('PIN funktioniert nicht?', 'link', true));
    } else {
      if (v.kind === 'expired') box.append(el('p', { className: 'booking-car', text: 'PIN abgelaufen' }));
      if (v.kind === 'invalid') box.append(el('p', { className: 'booking-car', text: 'Keine gültige PIN' }));
      box.append(act('Neue PIN holen', 'btn', false));
    }
  }
  box.append(fail);
  return box;
}

// Live km and price of running trips: refreshed every 60 s while the lines are on screen and the
// page is visible; stops by itself when the view is left.
const LIVE_REFRESH_MS = 60000;

function setLive(p, live) {
  const text = liveLine(live, Math.floor(Date.now() / 1000));
  p.textContent = text || '';
  p.hidden = !text;
}

function refreshLive(lines) {
  const timer = setInterval(async () => {
    if (![...lines.values()].some((p) => p.isConnected)) { clearInterval(timer); return; }
    if (document.hidden) return;
    try {
      for (const b of await api('GET', '/api/bookings')) {
        const p = lines.get(b.id);
        if (p && b.state === 'active') setLive(p, b.live);
      }
    } catch { /* keep the last values; next tick tries again */ }
  }, LIVE_REFRESH_MS);
}

// Error from a failed trip step; shown after the list reloads.
let carriedError = '';

export async function render(root) {
  const [list, cars] = await Promise.all([api('GET', '/api/bookings'), api('GET', '/api/cars')]);
  const names = new Map(cars.map((c) => [c.id, c.name]));
  const now = Math.floor(Date.now() / 1000);
  const upcoming = list.filter((b) => !b.cancelled_at && b.state !== 'cancelled')
    .sort((a, b) => a.booked_start - b.booked_start);

  const err = el('p', { className: 'error' });
  err.setAttribute('role', 'alert');
  err.hidden = !carriedError;
  err.textContent = carriedError;
  carriedError = '';
  root.append(el('h1', { text: 'Meine Buchungen' }), err);
  if (!upcoming.length) {
    root.append(el('p', { className: 'placeholder', text: 'Keine anstehenden Buchungen.' }),
      el('a', { className: 'btn primary', href: '#/kalender', text: 'Zum Kalender' }));
    return;
  }
  const liveLines = new Map();
  for (const b of upcoming) {
    const probe = b.state === 'active' ? await probePin(b) : null;
    const hasBox = !!probe && !probe.none;
    const act = tripActions(b, now);
    const step = (label, className, run) => {
      const btn = el('button', { type: 'button', className, text: label });
      btn.addEventListener('click', async () => {
        err.hidden = true;
        btn.disabled = true;
        try {
          if (!await run()) { btn.disabled = false; return; }
        } catch (x) {
          carriedError = x.message;
        }
        navigate('#/buchungen');
      });
      return btn;
    };
    const actions = [];
    if (act.start) actions.push(step('Starten', 'primary', () => runTrip(b, 'start', 'Fahrt starten', 'start')));
    if (act.ret) actions.push(step('Zurückgeben', 'primary', () => runTrip(b, 'return', 'Fahrzeug zurückgeben', 'end', hasBox)));
    if (act.report) actions.push(step('Schaden melden', 'btn', () => runReport(b)));
    if (act.extend) actions.push(el('a', { className: 'btn', href: `#/buchen?id=${b.id}`, text: 'Verlängern' }));
    if (act.cancel) {
      const del = el('button', { type: 'button', className: 'danger', text: 'Stornieren' });
      del.addEventListener('click', async () => {
        err.hidden = true;
        try {
          const s = await api('GET', '/api/settings/public');
          const inWindow = b.booked_start - Math.floor(Date.now() / 1000) < s.cancel_deadline_min * 60;
          const fee = !b.club_trip && s.cancel_fee_cents > 0 && inWindow ? formatEuro(s.cancel_fee_cents) : 'kostenlos';
          const ok = await confirmDialog(
            `Buchung ${formatDateTime(b.booked_start)} stornieren? Stornierung: ${fee}.`);
          if (!ok) return;
          await api('DELETE', `/api/bookings/${b.id}`);
          navigate('#/buchungen');
        } catch (x) {
          err.textContent = x.message;
          err.hidden = false;
        }
      });
      actions.push(del);
    }
    const badges = [];
    if (b.state === 'active') badges.push({ text: 'Läuft', kind: 'ok' });
    if (b.club_trip) badges.push({ text: 'Vereinsfahrt' });
    const line = subline(b, now, names.get(b.car_id), cars.length);
    const lines = line ? [line] : [];
    if (b.state === 'active') {
      const p = el('p', { className: 'booking-car live' });
      setLive(p, b.live);
      liveLines.set(b.id, p);
      lines.push(p);
    }
    if (hasBox) lines.push(pinLine(b, now, err, probe));
    root.append(bookingCard({
      start: b.booked_start, end: b.booked_end, nowSec: now, badges, lines, actions,
    }));
  }
  if (liveLines.size) refreshLive(liveLines);
}
