import { api } from '../api.js';
import { el, adminPage, pageHead, messageBox, textField, primaryButton } from '../admin_ui.js';
import { parseEuroToCents, formatEuro } from '../money.js';

const toEuroInput = (cents) => formatEuro(cents).replace(' €', '');

export async function render(root) {
  const s = await api('GET', '/api/admin/settings');
  const tol = textField('Starttoleranz (Minuten)', String(s.start_tolerance_min), { inputMode: 'numeric', required: true });
  const dead = textField('Stornofrist (Minuten vor Start)', String(s.cancel_deadline_min), { inputMode: 'numeric', required: true });
  const fee = textField('Stornogebühr (€)', toEuroInput(s.cancel_fee_cents), { inputMode: 'decimal', required: true });
  const rem = textField('Erinnerung vor Fahrtbeginn (Minuten, 0 = aus)', String(s.reminder_before_min), { inputMode: 'numeric', required: true });
  const aname = textField('Vereinsname', s.assoc_name, { maxLength: 95 });
  const astreet = textField('Straße', s.assoc_street, { maxLength: 95 });
  const azip = textField('PLZ', s.assoc_zip, { maxLength: 15 });
  const acity = textField('Ort', s.assoc_city, { maxLength: 63 });
  const cid = textField('Gläubiger-ID', s.creditor_id, { maxLength: 35 });
  const ciban = textField('IBAN des Vereins', s.creditor_iban, { maxLength: 50 });
  const cbic = textField('BIC des Vereins (optional)', s.creditor_bic, { maxLength: 20 });
  const lead = textField('Vorlauf Lastschrift (Tage)', String(s.sepa_lead_days), { inputMode: 'numeric', required: true });
  const footer = el('textarea', { value: s.invoice_footer, maxLength: 500, rows: 3 });
  const footerNode = el('label', {}, 'Rechnungsfußzeile', footer);
  const err = messageBox('error');
  const ok = messageBox('success');
  const sepaCard = el('div', { className: 'card' },
    el('h2', { text: 'Verein und Lastschrift' }),
    aname.node, astreet.node, azip.node, acity.node, cid.node, ciban.node, cbic.node, lead.node, footerNode);
  const form = el('form', { className: 'card' },
    el('h2', { text: 'Buchungsregeln' }), tol.node, dead.node, fee.node, rem.node, sepaCard,
    err.node, ok.node, primaryButton('Speichern'));
  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    err.hide(); ok.hide();
    const minutes = (v) => (/^\d{1,9}$/.test(v.trim()) ? Number(v.trim()) : null);
    const t = minutes(tol.input.value), d = minutes(dead.input.value);
    const cents = parseEuroToCents(fee.input.value);
    if (t === null) return err.show('Starttoleranz: bitte eine ganze Zahl von Minuten eingeben.');
    if (d === null) return err.show('Stornofrist: bitte eine ganze Zahl von Minuten eingeben.');
    const r0 = minutes(rem.input.value);
    if (r0 === null || r0 > 1440) return err.show('Erinnerung: bitte 0 bis 1440 Minuten eingeben.');
    const l = minutes(lead.input.value);
    if (l === null || l < 5 || l > 30) return err.show('Vorlauf Lastschrift: bitte 5 bis 30 Tage eingeben.');
    if (new TextEncoder().encode(footer.value).length >= 512) return err.show('Rechnungsfußzeile: höchstens 511 Bytes.');
    if (cents === null) return err.show('Stornogebühr: bitte einen Eurobetrag wie 5 oder 5,50 eingeben.');
    try {
      const r = await api('PUT', '/api/admin/settings',
        {
          start_tolerance_min: t, cancel_deadline_min: d, cancel_fee_cents: cents,
          assoc_name: aname.input.value.trim(), assoc_street: astreet.input.value.trim(),
          assoc_zip: azip.input.value.trim(), assoc_city: acity.input.value.trim(),
          creditor_id: cid.input.value.trim(), creditor_iban: ciban.input.value.trim(),
          creditor_bic: cbic.input.value.trim(),
          sepa_lead_days: l, invoice_footer: footer.value, reminder_before_min: r0,
        });
      tol.input.value = String(r.start_tolerance_min);
      dead.input.value = String(r.cancel_deadline_min);
      fee.input.value = toEuroInput(r.cancel_fee_cents);
      rem.input.value = String(r.reminder_before_min);
      aname.input.value = r.assoc_name;
      astreet.input.value = r.assoc_street;
      azip.input.value = r.assoc_zip;
      acity.input.value = r.assoc_city;
      cid.input.value = r.creditor_id;
      ciban.input.value = r.creditor_iban;
      cbic.input.value = r.creditor_bic;
      lead.input.value = String(r.sepa_lead_days);
      footer.value = r.invoice_footer;
      ok.show('Gespeichert.');
    } catch (x) { err.show(x.message); }
  });
  adminPage(root, '#/admin/einstellungen').append(pageHead('Einstellungen'), form);
}
