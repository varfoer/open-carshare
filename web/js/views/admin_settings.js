import { api } from '../api.js';
import { el, adminNav, messageBox, textField, primaryButton } from '../admin_ui.js';
import { parseEuroToCents, formatEuro } from '../money.js';

const toEuroInput = (cents) => formatEuro(cents).replace(' €', '');

export async function render(root) {
  const s = await api('GET', '/api/admin/settings');
  const tol = textField('Starttoleranz (Minuten)', String(s.start_tolerance_min), { inputMode: 'numeric', required: true });
  const dead = textField('Stornofrist (Minuten vor Start)', String(s.cancel_deadline_min), { inputMode: 'numeric', required: true });
  const fee = textField('Stornogebühr (€)', toEuroInput(s.cancel_fee_cents), { inputMode: 'decimal', required: true });
  const err = messageBox('error');
  const ok = messageBox('success');
  const form = el('form', { className: 'card' },
    el('h1', { text: 'Einstellungen' }), tol.node, dead.node, fee.node, err.node, ok.node, primaryButton('Speichern'));
  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    err.hide(); ok.hide();
    const minutes = (v) => (/^\d{1,9}$/.test(v.trim()) ? Number(v.trim()) : null);
    const t = minutes(tol.input.value), d = minutes(dead.input.value);
    const cents = parseEuroToCents(fee.input.value);
    if (t === null) return err.show('Starttoleranz: bitte eine ganze Zahl von Minuten eingeben.');
    if (d === null) return err.show('Stornofrist: bitte eine ganze Zahl von Minuten eingeben.');
    if (cents === null) return err.show('Stornogebühr: bitte einen Eurobetrag wie 5 oder 5,50 eingeben.');
    try {
      const r = await api('PUT', '/api/admin/settings',
        { start_tolerance_min: t, cancel_deadline_min: d, cancel_fee_cents: cents });
      tol.input.value = String(r.start_tolerance_min);
      dead.input.value = String(r.cancel_deadline_min);
      fee.input.value = toEuroInput(r.cancel_fee_cents);
      ok.show('Gespeichert.');
    } catch (x) { err.show(x.message); }
  });
  root.append(adminNav('#/admin/einstellungen'), form);
}
