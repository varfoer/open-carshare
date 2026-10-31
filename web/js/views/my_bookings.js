import { api } from '../api.js';
import { navigate } from '../app.js';
import { formatDateTime } from '../time.js';
import { formatEuro } from '../money.js';
import { tripActions } from '../trip.js';
import { apiUpload, resizeToJpeg } from '../photo.js';
import { damageDialog } from './damage_dialog.js';

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
async function runTrip(b, action, title, phase) {
  const damage = await damageDialog(title);
  if (damage === null) return false;
  await api('POST', `/api/bookings/${b.id}/${action}`);
  if (damage === 'none') return true;
  try {
    await fileReport(b, phase, damage);
  } catch (x) {
    if (phase === 'start') retryText.set(b.id, damage.text);
    const done = phase === 'start' ? 'Fahrt gestartet' : 'Fahrzeug zurückgegeben';
    throw new Error(`${done}, aber die Schadensmeldung ist fehlgeschlagen: ${x.message.replace(/\.$/, '')}. Bitte erneut melden.`);
  }
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
    const dlg = document.createElement('dialog');
    dlg.className = 'confirm';
    const p = document.createElement('p');
    p.textContent = text;
    const row = document.createElement('div');
    row.className = 'dialog-actions';
    const no = document.createElement('button');
    no.type = 'button';
    no.textContent = 'Zurück';
    const yes = document.createElement('button');
    yes.type = 'button';
    yes.className = 'danger';
    yes.textContent = 'Stornieren';
    row.append(no, yes);
    dlg.append(p, row);
    let result = false;
    no.addEventListener('click', () => dlg.close());
    yes.addEventListener('click', () => { result = true; dlg.close(); });
    dlg.addEventListener('close', () => { dlg.remove(); resolve(result); });
    document.body.append(dlg);
    dlg.showModal();
  });
}

// Error from a failed trip step; shown after the list reloads.
let carriedError = '';

export async function render(root) {
  const [list, cars] = await Promise.all([api('GET', '/api/bookings'), api('GET', '/api/cars')]);
  const names = new Map(cars.map((c) => [c.id, c.name]));
  const now = Math.floor(Date.now() / 1000);
  const upcoming = list.filter((b) => !b.cancelled_at && b.state !== 'cancelled')
    .sort((a, b) => a.booked_start - b.booked_start);

  const h1 = document.createElement('h1');
  h1.textContent = 'Meine Buchungen';
  const err = document.createElement('p');
  err.className = 'error';
  err.setAttribute('role', 'alert');
  err.hidden = !carriedError;
  err.textContent = carriedError;
  carriedError = '';
  root.append(h1, err);
  if (!upcoming.length) {
    const p = document.createElement('p');
    p.className = 'placeholder';
    p.textContent = 'Keine anstehenden Buchungen.';
    root.append(p);
    return;
  }
  for (const b of upcoming) {
    const card = document.createElement('div');
    card.className = 'card booking';
    const t = document.createElement('p');
    t.className = 'booking-time';
    t.textContent = `${formatDateTime(b.booked_start)} – ${formatDateTime(b.booked_end)}`;
    const c = document.createElement('p');
    c.className = 'booking-car';
    c.textContent = names.get(b.car_id) || '';
    if (b.club_trip) {
      const badge = document.createElement('span');
      badge.className = 'badge';
      badge.textContent = 'Vereinsfahrt';
      c.append(' ', badge);
    }
    const act = tripActions(b, now);
    const actions = document.createElement('div');
    actions.className = 'dialog-actions';
    const step = (label, className, run) => {
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = className;
      btn.textContent = label;
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
    if (act.start) actions.append(step('Starten', 'primary', () => runTrip(b, 'start', 'Fahrt starten', 'start')));
    if (act.ret) actions.append(step('Zurückgeben', 'primary', () => runTrip(b, 'return', 'Fahrzeug zurückgeben', 'end')));
    if (act.report) actions.append(step('Schaden melden', 'danger', () => runReport(b)));
    const since = document.createElement('p');
    since.className = 'booking-car';
    since.textContent = `Unterwegs seit ${formatDateTime(b.actual_start)}`;
    since.hidden = b.state !== 'active';
    if (act.extend) {
      const ext = document.createElement('a');
      ext.className = 'btn';
      ext.href = `#/buchen?id=${b.id}`;
      ext.textContent = 'Verlängern';
      actions.append(ext);
    }
    if (act.cancel) {
      const del = document.createElement('button');
      del.type = 'button';
      del.className = 'danger';
      del.textContent = 'Stornieren';
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
      actions.append(del);
    }
    card.append(t, c, since, actions);
    root.append(card);
  }
}
