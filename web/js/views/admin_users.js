import { api } from '../api.js';
import { el, adminNav, messageBox, textField, checkField, selectField, primaryButton } from '../admin_ui.js';
import { berlinToEpoch, epochToBerlin } from '../time.js';
import { dateStr } from '../form_time.js';

export async function render(root) {
  const users = await api('GET', '/api/admin/users');
  const err = messageBox('error');
  const ok = messageBox('success');
  const table = el('table', { className: 'data' });
  const formHost = el('div');
  const add = el('button', { type: 'button', className: 'primary', text: 'Nutzer anlegen' });
  root.append(adminNav('#/admin/nutzer'), el('h1', { text: 'Nutzer' }), err.node, ok.node,
    el('div', { className: 'table-wrap' }, table), add, formHost);

  const reload = async (msg) => {
    root.replaceChildren();
    await render(root);
    if (msg) {
      const m = root.querySelector('p.success');
      m.textContent = msg;
      m.hidden = false;
    }
  };

  const head = el('tr');
  for (const h of ['Name', 'E-Mail', 'Rolle', 'aktiv', 'Vereinsfahrten', '']) head.append(el('th', { text: h }));
  table.append(el('thead', {}, head));
  const body = el('tbody');
  table.append(body);
  for (const u of users) {
    const edit = el('button', { type: 'button', text: 'Bearbeiten' });
    edit.addEventListener('click', () => showForm(u));
    const inv = el('button', { type: 'button', text: 'Einladung erneut senden' });
    inv.addEventListener('click', async () => {
      err.hide(); ok.hide();
      inv.disabled = true;
      try {
        await api('POST', `/api/admin/users/${u.id}/invite`);
        ok.show(`Einladung an ${u.email} gesendet.`);
      } catch (x) { err.show(x.message); }
      inv.disabled = false;
    });
    body.append(el('tr', {},
      el('td', { text: u.name }), el('td', { text: u.email }),
      el('td', { text: u.role === 'admin' ? 'Admin' : 'Nutzer' }),
      el('td', { text: u.active ? 'ja' : 'nein' }),
      el('td', { text: u.may_club_trip ? 'ja' : 'nein' }),
      el('td', { className: 'actions' }, edit, inv)));
  }

  function showForm(u) {
    const email = textField('E-Mail', u ? u.email : '', { type: 'email', required: true });
    const name = textField('Name', u ? u.name : '', { required: true });
    const street = textField('Straße', u ? u.street : '');
    const zip = textField('PLZ', u ? u.zip : '');
    const city = textField('Ort', u ? u.city : '');
    const role = selectField('Rolle', [['user', 'Nutzer'], ['admin', 'Admin']], u ? u.role : 'user');
    const active = checkField('aktiv', u ? u.active : true);
    const origDate = u && u.license_checked_at ? dateStr(epochToBerlin(u.license_checked_at)) : '';
    const lic = textField('Führerschein geprüft am', origDate, { type: 'date' });
    const club = checkField('Darf Vereinsfahrten buchen', u ? u.may_club_trip : false);
    const fErr = messageBox('error');
    const cancel = el('button', { type: 'button', text: 'Abbrechen', className: 'btn' });
    cancel.addEventListener('click', () => formHost.replaceChildren());
    const form = el('form', { className: 'card' },
      el('h2', { text: u ? 'Nutzer bearbeiten' : 'Nutzer anlegen' }),
      email.node, name.node, street.node, zip.node, city.node, role.node,
      ...(u ? [active.node] : []), club.node, lic.node, fErr.node,
      primaryButton(u ? 'Speichern' : 'Anlegen und einladen'), cancel);
    form.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      fErr.hide(); err.hide(); ok.hide();
      const body = {
        email: email.input.value.trim(), name: name.input.value.trim(),
        street: street.input.value.trim(), zip: zip.input.value.trim(), city: city.input.value.trim(),
        role: role.input.value, may_club_trip: club.input.checked,
      };
      // Optional: send only when set and (on edit) changed, else the server keeps the old value.
      if (lic.input.value && lic.input.value !== origDate) {
        const [y, mo, d] = lic.input.value.split('-').map(Number);
        body.license_checked_at = berlinToEpoch(y, mo, d, 0, 0);
      }
      try {
        if (u) {
          body.active = active.input.checked;
          await api('PUT', `/api/admin/users/${u.id}`, body);
          await reload('Gespeichert.');
        } else {
          const r = await api('POST', '/api/admin/users', body);
          await reload(r.mail_sent
            ? `Nutzer angelegt, Einladung gesendet.`
            : 'Nutzer angelegt, aber die Einladung konnte nicht gesendet werden. Bitte erneut senden.');
        }
      } catch (x) { fErr.show(x.message); }
    });
    formHost.replaceChildren(form);
    form.scrollIntoView();
  }
  add.addEventListener('click', () => showForm(null));
}
