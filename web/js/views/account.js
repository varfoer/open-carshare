import { api } from '../api.js';
import { refreshUser, navigate } from '../app.js';
import { el, messageBox, textField, checkField, primaryButton } from '../admin_ui.js';
import { epochToBerlin } from '../time.js';
import { p2 } from '../form_time.js';
import { ibanValid, formatIban, normalizeIban } from '../sepa.js';

const MIN_PASSWORD = 10;

const berlinDate = (ts) => {
  const b = epochToBerlin(ts);
  return `${p2(b.d)}.${p2(b.mo)}.${b.y}`;
};

function mandateCard(user) {
  const card = el('section', { className: 'card' });
  const ok = messageBox('success');

  async function load(flash) {
    let data;
    try { data = await api('GET', '/api/me/mandate'); } catch (x) {
      card.replaceChildren(el('h2', { text: 'SEPA-Lastschriftmandat' }), el('p', { className: 'error', text: x.message }));
      return;
    }
    card.replaceChildren(el('h2', { text: 'SEPA-Lastschriftmandat' }));
    if (!data.ready) {
      card.append(el('p', { className: 'hint', text: 'Lastschriftmandate sind noch nicht eingerichtet.' }));
      return;
    }
    if (flash) { ok.show(flash); card.append(ok.node); } else ok.hide();
    const m = data.mandate;
    if (m) {
      const dl = el('dl', { className: 'kv' });
      for (const [k, v] of [['Mandatsreferenz', m.ref], ['Kontoinhaber', m.holder], ['IBAN', m.iban_masked], ['Erteilt am', berlinDate(m.created_at)]]) {
        dl.append(el('dt', { text: k }), el('dd', { text: v }));
      }
      const formArea = el('div');
      const revokeArea = el('div');
      const change = el('button', { type: 'button', className: 'btn', text: 'Bankverbindung ändern' });
      const revoke = el('button', { type: 'button', className: 'danger', text: 'Mandat widerrufen' });
      change.addEventListener('click', () => { revokeArea.replaceChildren(); formArea.replaceChildren(giveForm(data)); });
      revoke.addEventListener('click', () => { formArea.replaceChildren(); revokeArea.replaceChildren(revokeForm()); });
      card.append(dl, el('div', { className: 'dialog-actions' }, change, revoke), formArea, revokeArea);
    } else {
      card.append(el('p', { className: 'hint', text: 'Ohne Mandat kannst du keine Fahrzeuge buchen.' }), giveForm(data));
    }
  }

  function giveForm(data) {
    const holder = textField('Kontoinhaber', user.name, { required: true, autocomplete: 'off' });
    const iban = textField('IBAN', '', { required: true, autocomplete: 'off', inputMode: 'text' });
    const bic = textField('BIC (für IBANs außerhalb des EWR erforderlich)', '', { autocomplete: 'off' });
    iban.input.addEventListener('blur', () => { iban.input.value = formatIban(iban.input.value); });
    const text = el('pre', { className: 'mandate-text', text: data.text || '' });
    const acc = checkField('Ich erteile dieses SEPA-Lastschriftmandat.', false);
    acc.input.required = true;
    const err = messageBox('error');
    const submit = primaryButton('Mandat erteilen');
    const f = el('form', {}, holder.node, iban.node, bic.node, text, acc.node, err.node, submit);
    f.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      err.hide();
      if (!ibanValid(iban.input.value)) return err.show('Die IBAN ist ungültig.');
      const body = {
        account_holder: holder.input.value.trim(), iban: normalizeIban(iban.input.value),
        creditor_id: data.creditor_id, text_version: data.text_version, accept: true,
      };
      const b = bic.input.value.trim();
      if (b) body.bic = b;
      submit.disabled = true;
      try {
        const r = await api('POST', '/api/me/mandate', body);
        await load(!r.changed ? 'Das Mandat ist unverändert.'
          : r.mail_sent
            ? 'Mandat erteilt. Die Bestätigung kommt per E-Mail.'
            : 'Mandat erteilt, die E-Mail konnte aber nicht gesendet werden.');
      } catch (x) { err.show(x.message); } finally { submit.disabled = false; }
    });
    return f;
  }

  function revokeForm() {
    const pw = textField('Passwort', '', { type: 'password', required: true, autocomplete: 'current-password' });
    const err = messageBox('error');
    const submit = el('button', { type: 'submit', className: 'danger', text: 'Endgültig widerrufen' });
    const f = el('form', {},
      el('p', { className: 'error', text: 'Dein Konto wird deaktiviert und deine offenen Reservierungen werden storniert. Du wirst abgemeldet.' }),
      pw.node, err.node, submit);
    f.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      err.hide();
      submit.disabled = true;
      try {
        await api('DELETE', '/api/me/mandate', { password: pw.input.value });
        await refreshUser();
        navigate('#/login');
      } catch (x) { err.show(x.message); } finally { submit.disabled = false; }
    });
    return f;
  }

  load();
  return card;
}

export async function render(root) {
  const { user } = await api('GET', '/api/me');

  // Profile
  const name = textField('Name', user.name, { required: true, autocomplete: 'name' });
  const pErr = messageBox('error');
  const pOk = messageBox('success');
  const pForm = el('form', { className: 'card' },
    el('h1', { text: 'Konto' }),
    el('p', { className: 'hint', text: user.email }),
    name.node, pErr.node, pOk.node, primaryButton('Speichern'));
  pForm.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    pErr.hide(); pOk.hide();
    try {
      await api('PUT', '/api/me', { name: name.input.value.trim() });
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

  root.append(pForm, mandateCard(user), wForm);
}
