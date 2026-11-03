import { api } from '../api.js';
import { currentUser } from '../app.js';
import { el, adminNav, messageBox, textField, checkField, selectField, primaryButton } from '../admin_ui.js';
import { berlinToEpoch, epochToBerlin } from '../time.js';
import { dateStr, p2 } from '../form_time.js';

const berlinDate = (ts) => {
  const b = epochToBerlin(ts);
  return `${p2(b.d)}.${p2(b.mo)}.${b.y}`;
};
const STATUS_DE = { active: 'aktiv', replaced: 'ersetzt', revoked: 'widerrufen' };

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
  for (const h of ['Name', 'E-Mail', 'Rolle', 'aktiv', 'Vereinsfahrten', 'Mandat', '']) head.append(el('th', { text: h }));
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
      el('td', { className: u.mandate ? '' : 'warn', text: u.mandate ? u.mandate.ref : 'fehlt' }),
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
    if (u) {
      const exp = el('button', { type: 'button', text: 'Daten exportieren', className: 'btn' });
      exp.addEventListener('click', async () => {
        fErr.hide();
        exp.disabled = true;
        try {
          const data = await api('GET', `/api/admin/users/${u.id}/export`);
          const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
          const url = URL.createObjectURL(blob);
          const a = el('a', { href: url, download: `ocs-export-${u.id}.json` });
          document.body.append(a);
          a.click();
          a.remove();
          setTimeout(() => URL.revokeObjectURL(url), 1000);
        } catch (x) { fErr.show(x.message); }
        exp.disabled = false;
      });
      form.append(exp, mandateSection(u));
    }
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
          if (u.active && !body.active &&
              !window.confirm('Konto deaktivieren? Offene Reservierungen werden kostenlos storniert.')) return;
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
  function mandateSection(u) {
    const sec = el('section', {}, el('h3', { text: 'Lastschriftmandate' }));
    const list = el('div');
    sec.append(list);
    const me = currentUser();
    (async () => {
      let rows;
      try { rows = await api('GET', `/api/admin/users/${u.id}/mandates`); } catch (x) {
        list.append(el('p', { className: 'error', text: x.message }));
        return;
      }
      if (!rows.length) { list.append(el('p', { className: 'hint', text: 'Kein Mandat erteilt.' })); return; }
      const t = el('table', { className: 'data' });
      const h = el('tr');
      for (const c of ['Referenz', 'Status', 'Kontoinhaber', 'IBAN', 'erteilt am', 'beendet am', 'IP', 'Textversion']) h.append(el('th', { text: c }));
      t.append(el('thead', {}, h));
      const tb = el('tbody');
      for (const m of rows) {
        tb.append(el('tr', {},
          el('td', { text: m.ref }), el('td', { text: STATUS_DE[m.status] || m.status }),
          el('td', { text: m.holder }), el('td', { text: m.iban_masked }),
          el('td', { text: berlinDate(m.created_at) }),
          el('td', { text: m.ended_at ? berlinDate(m.ended_at) : '' }),
          el('td', { text: m.client_ip }), el('td', { text: String(m.text_version) })));
      }
      t.append(tb);
      list.append(el('div', { className: 'table-wrap' }, t));
      if (u.mandate && !(me && me.id === u.id)) {
        const rev = el('button', { type: 'button', className: 'danger', text: 'Mandat widerrufen' });
        rev.addEventListener('click', async () => {
          if (!window.confirm('Mandat widerrufen? Das Konto wird deaktiviert und offene Reservierungen werden storniert.')) return;
          err.hide(); ok.hide();
          rev.disabled = true;
          try {
            await api('POST', `/api/admin/users/${u.id}/mandate/revoke`);
            await reload('Mandat widerrufen.');
          } catch (x) { err.show(x.message); rev.disabled = false; }
        });
        list.append(rev);
      }
    })();
    return sec;
  }
  add.addEventListener('click', () => showForm(null));
}
