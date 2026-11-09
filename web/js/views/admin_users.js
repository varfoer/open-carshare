import { api } from '../api.js';
import { currentUser } from '../app.js';
import { el, adminNav, messageBox, textField, checkField, selectField, primaryButton } from '../admin_ui.js';
import { berlinToEpoch, epochToBerlin, formatDateTime } from '../time.js';
import { dateStr, p2, dateTime, toEpoch } from '../form_time.js';

const berlinDate = (ts) => {
  const b = epochToBerlin(ts);
  return `${p2(b.d)}.${p2(b.mo)}.${b.y}`;
};
const STATUS_DE = { active: 'aktiv', replaced: 'ersetzt', revoked: 'widerrufen' };

export async function render(root) {
  const users = await api('GET', '/api/admin/users');
  const tariffs = await api('GET', '/api/admin/tariffs');
  const tariffName = (id) => (tariffs.find((t) => t.id === id) || {}).name || 'keiner';
  const tariffOptions = tariffs.map((t) => [String(t.id), t.name]);
  const now = Math.floor(Date.now() / 1000);
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
  for (const h of ['Name', 'E-Mail', 'Rolle', 'aktiv', 'Vereinsfahrten', 'Tarif', 'Mandat', '']) head.append(el('th', { text: h }));
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
      el('td', { className: u.tariff_id === 0 ? 'warn' : '', text: tariffName(u.tariff_id) }),
      el('td', { className: u.mandate ? '' : 'warn', text: u.mandate ? u.mandate.ref : 'fehlt' }),
      el('td', { className: 'actions' }, edit, inv)));
  }

  function showForm(u) {
    const email = textField('E-Mail', u ? u.email : '', { type: 'email', required: true });
    const name = textField('Name', u ? u.name : '', { required: true });
    const street = textField('Straße (optional)', u ? u.street : '');
    const zip = textField('PLZ (optional)', u ? u.zip : '');
    const city = textField('Ort (optional)', u ? u.city : '');
    const role = selectField('Rolle', [['user', 'Nutzer'], ['admin', 'Admin']], u ? u.role : 'user');
    const active = checkField('aktiv', u ? u.active : true);
    const origDate = u && u.license_checked_at ? dateStr(epochToBerlin(u.license_checked_at)) : '';
    const lic = textField('Führerschein geprüft am', origDate, { type: 'date' });
    const club = checkField('Darf Vereinsfahrten buchen', u ? u.may_club_trip : false);
    // Create only: the initial tariff; existing users change it via tariffSection.
    const tariffSel = selectField('Tarif', tariffOptions, tariffs[0] ? String(tariffs[0].id) : '');
    const fErr = messageBox('error');
    const submit = primaryButton(u ? 'Speichern' : 'Anlegen und einladen');
    const noTariff = !u && !tariffs.length;
    if (noTariff) submit.disabled = true;
    const cancel = el('button', { type: 'button', text: 'Abbrechen', className: 'btn' });
    cancel.addEventListener('click', () => formHost.replaceChildren());
    const form = el('form', { className: 'card' },
      el('h2', { text: u ? 'Nutzer bearbeiten' : 'Nutzer anlegen' }),
      email.node, name.node, street.node, zip.node, city.node, role.node,
      ...(u ? [active.node] : []), club.node,
      ...(u ? [] : [tariffSel.node]),
      ...(noTariff ? [el('p', { className: 'hint', text: 'Bitte zuerst einen Tarif anlegen.' })] : []),
      lic.node, fErr.node, submit, cancel);
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
      form.append(exp, tariffSection(u, fErr), mandateSection(u));
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
          body.tariff_id = Number(tariffSel.input.value);
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
  // Tariff assignments of an existing user, newest first; only the newest may be edited or deleted.
  // Buttons, not a nested form: the section lives inside the user form.
  function tariffSection(u, fErr) {
    const sec = el('section', {}, el('h3', { text: 'Tarif' }));
    const list = el('div');
    sec.append(list);
    const run = async (btn, method, path, body) => {
      fErr.hide(); err.hide(); ok.hide();
      btn.disabled = true;
      try {
        await api(method, path, body);
        await reload('Gespeichert.');
      } catch (x) { fErr.show(x.message); btn.disabled = false; }
    };
    (async () => {
      let rows;
      try { rows = await api('GET', `/api/admin/users/${u.id}/tariffs`); } catch (x) {
        list.append(el('p', { className: 'error', text: x.message }));
        return;
      }
      const inForce = rows.reduce((f, a) => (a.valid_from <= now && (!f || a.valid_from > f.valid_from) ? a : f), null);
      if (!rows.length) list.append(el('p', { className: 'hint', text: 'Tarif: keiner' }));
      rows.forEach((a, i) => {
        const line = el('p', {}, `ab ${formatDateTime(a.valid_from)}: ${tariffName(a.tariff_id)}${a === inForce ? ' (gilt)' : ''}`);
        list.append(line);
        if (i !== 0) return;
        const editBtn = el('button', { type: 'button', text: 'Bearbeiten', className: 'btn' });
        const del = el('button', { type: 'button', text: 'Löschen', className: 'danger' });
        line.append(' ', editBtn, ' ', del);
        const sel = selectField('Tarif', tariffOptions, String(a.tariff_id));
        const when = dateTime(a.valid_from);
        const save = el('button', { type: 'button', text: 'Speichern', className: 'btn' });
        const editor = el('div', {}, sel.node, el('label', {}, 'Gültig ab', when.date, when.time), save);
        editor.hidden = true;
        list.append(editor);
        editBtn.addEventListener('click', () => { editor.hidden = !editor.hidden; });
        save.addEventListener('click', () => {
          if (!when.date.value) { fErr.show('Bitte ein Datum angeben.'); return; }
          run(save, 'PUT', `/api/admin/user-tariffs/${a.id}`,
            { tariff_id: Number(sel.input.value), valid_from: toEpoch(when) });
        });
        del.addEventListener('click', () => {
          if (!window.confirm('Tarifzuordnung löschen?')) return;
          run(del, 'DELETE', `/api/admin/user-tariffs/${a.id}`);
        });
      });
      if (!tariffs.length) return;
      const sel = selectField('Tarif', tariffOptions, String(inForce ? inForce.tariff_id : tariffs[0].id));
      const when = dateTime(Math.ceil((now + 900) / 900) * 900);
      const go = el('button', { type: 'button', text: 'Tarif wechseln', className: 'btn' });
      go.addEventListener('click', () => {
        if (!when.date.value) { fErr.show('Bitte ein Datum angeben.'); return; }
        run(go, 'POST', `/api/admin/users/${u.id}/tariffs`,
          { tariff_id: Number(sel.input.value), valid_from: toEpoch(when) });
      });
      list.append(el('div', {}, el('h4', { text: 'Tarif wechseln' }), sel.node,
        el('label', {}, 'Gültig ab', when.date, when.time), go));
    })();
    return sec;
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
