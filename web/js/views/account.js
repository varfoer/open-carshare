import { api } from '../api.js';
import { refreshUser } from '../app.js';
import { el, messageBox, textField, primaryButton } from '../admin_ui.js';

const MIN_PASSWORD = 10;

export async function render(root) {
  const { user } = await api('GET', '/api/me');

  // Profile
  const name = textField('Name', user.name, { required: true, autocomplete: 'name' });
  const street = textField('Straße', user.street, { autocomplete: 'street-address' });
  const zip = textField('PLZ', user.zip, { autocomplete: 'postal-code' });
  const city = textField('Ort', user.city, { autocomplete: 'address-level2' });
  const pErr = messageBox('error');
  const pOk = messageBox('success');
  const pForm = el('form', { className: 'card' },
    el('h1', { text: 'Konto' }),
    el('p', { className: 'hint', text: user.email }),
    name.node, street.node, zip.node, city.node, pErr.node, pOk.node, primaryButton('Speichern'));
  pForm.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    pErr.hide(); pOk.hide();
    try {
      await api('PUT', '/api/me', {
        name: name.input.value.trim(), street: street.input.value.trim(),
        zip: zip.input.value.trim(), city: city.input.value.trim(),
      });
      await refreshUser();
      pOk.show('Gespeichert.');
    } catch (x) { pErr.show(x.message); }
  });

  // Password
  const cur = textField('Aktuelles Passwort', '', { type: 'password', required: true, autocomplete: 'current-password' });
  const nw = textField('Neues Passwort', '', { type: 'password', required: true, autocomplete: 'new-password', minLength: MIN_PASSWORD });
  const rep = textField('Neues Passwort wiederholen', '', { type: 'password', required: true, autocomplete: 'new-password' });
  const wErr = messageBox('error');
  const wOk = messageBox('success');
  const wForm = el('form', { className: 'card' },
    el('h2', { text: 'Passwort ändern' }),
    el('p', { className: 'hint', text: `Mindestens ${MIN_PASSWORD} Zeichen.` }),
    cur.node, nw.node, rep.node, wErr.node, wOk.node, primaryButton('Passwort ändern'));
  wForm.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    wErr.hide(); wOk.hide();
    if (nw.input.value.length < MIN_PASSWORD) return wErr.show(`Das neue Passwort muss mindestens ${MIN_PASSWORD} Zeichen lang sein.`);
    if (nw.input.value !== rep.input.value) return wErr.show('Die neuen Passwörter stimmen nicht überein.');
    try {
      await api('PUT', '/api/me/password', { current: cur.input.value, new: nw.input.value });
      wForm.reset();
      wOk.show('Passwort geändert.');
    } catch (x) { wErr.show(x.message); }
  });

  root.append(pForm, wForm);
}
