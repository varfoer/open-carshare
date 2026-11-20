import { api } from '../api.js';
import { pinView } from '../trip.js';
import { el } from '../admin_ui.js';

// Return step for a car with a key box: shows the PIN (fetching a new one if none is usable)
// and asks to put the key back. Resolves true to continue with the return.
export function pinDialog(bookingId) {
  return new Promise((resolve) => {
    const dlg = el('dialog', { className: 'confirm' });
    const body = el('div', {});
    const actions = el('div', { className: 'dialog-actions' });
    dlg.append(el('h2', { text: 'Schlüsselbox' }), body, actions);
    let result = false;
    const button = (text, className, onClick) => {
      const b = el('button', { type: 'button', className, text });
      b.addEventListener('click', onClick);
      return b;
    };
    const cancel = () => button('Abbrechen', '', () => dlg.close());
    const go = (text, className) => button(text, className, () => { result = true; dlg.close(); });
    body.append(el('p', { text: 'PIN wird geladen…' }));
    actions.append(cancel());
    dlg.addEventListener('close', () => { dlg.remove(); resolve(result); });
    dlg.addEventListener('cancel', () => { result = false; });
    document.body.append(dlg);
    dlg.showModal();

    (async () => {
      const now = () => Math.floor(Date.now() / 1000);
      let v = pinView(await api('GET', `/api/bookings/${bookingId}/pin`), now());
      if (v.kind !== 'show') {
        v = pinView(await api('POST', `/api/bookings/${bookingId}/pin`, { renew: false }), now());
      }
      if (v.kind !== 'show') throw new Error('PIN für die Schlüsselbox konnte nicht abgerufen werden.');
      body.replaceChildren(
        el('p', { className: 'pin-digits', text: v.pin }),
        el('p', { text: 'Schlüssel in die Box legen und Box schließen.' }));
      actions.replaceChildren(cancel(), go('Weiter', 'primary'));
    })().catch((x) => {
      body.replaceChildren(el('p', { className: 'error', text: x.message }));
      actions.replaceChildren(cancel(), go('Trotzdem zurückgeben', 'danger'));
    });
  });
}
