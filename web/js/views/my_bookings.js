import { api } from '../api.js';
import { navigate } from '../app.js';
import { formatDateTime } from '../time.js';
import { formatEuro } from '../money.js';

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

export async function render(root) {
  const [list, cars] = await Promise.all([api('GET', '/api/bookings'), api('GET', '/api/cars')]);
  const names = new Map(cars.map((c) => [c.id, c.name]));
  const upcoming = list.filter((b) => !b.cancelled_at && b.state !== 'cancelled')
    .sort((a, b) => a.booked_start - b.booked_start);

  const h1 = document.createElement('h1');
  h1.textContent = 'Meine Buchungen';
  const err = document.createElement('p');
  err.className = 'error';
  err.setAttribute('role', 'alert');
  err.hidden = true;
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
    const actions = document.createElement('div');
    actions.className = 'dialog-actions';
    const ext = document.createElement('a');
    ext.className = 'btn';
    ext.href = `#/buchen?id=${b.id}`;
    ext.textContent = 'Verlängern';
    const del = document.createElement('button');
    del.type = 'button';
    del.className = 'danger';
    del.textContent = 'Stornieren';
    del.addEventListener('click', async () => {
      err.hidden = true;
      try {
        const s = await api('GET', '/api/settings/public');
        const inWindow = b.booked_start - Math.floor(Date.now() / 1000) < s.cancel_deadline_min * 60;
        const fee = s.cancel_fee_cents > 0 && inWindow ? formatEuro(s.cancel_fee_cents) : 'kostenlos';
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
    actions.append(ext, del);
    card.append(t, c, actions);
    root.append(card);
  }
}
