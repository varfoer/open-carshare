import { el } from '../admin_ui.js';

const MAX_PHOTOS = 10;

// Resolves null (abort), 'none' (no damage) or { text, files }.
export function damageDialog(title) {
  return new Promise((resolve) => {
    const dlg = el('dialog', { className: 'confirm' });
    const err = el('p', { className: 'error' });
    err.setAttribute('role', 'alert');
    err.hidden = true;
    const text = el('textarea', { rows: 4, required: true, maxLength: 1000 });
    const files = el('input', { type: 'file', accept: 'image/*', multiple: true });
    const form = el('form', { hidden: true },
      el('label', { text: 'Beschreibung des Schadens' }, text),
      el('label', { text: 'Fotos (höchstens 10)' }, files),
      err);
    const cancel = el('button', { type: 'button', text: 'Abbrechen' });
    const none = el('button', { type: 'button', className: 'primary', text: 'Keine Schäden' });
    const report = el('button', { type: 'button', className: 'danger', text: 'Schaden melden' });
    const send = el('button', { type: 'submit', className: 'danger', text: 'Melden' });
    const row = el('div', { className: 'dialog-actions' }, cancel, none, report);
    form.append(el('div', { className: 'dialog-actions' }, send));
    dlg.append(el('h2', { text: title }), form, row);
    let result = null;
    cancel.addEventListener('click', () => dlg.close());
    none.addEventListener('click', () => { result = 'none'; dlg.close(); });
    report.addEventListener('click', () => {
      form.hidden = false;
      row.hidden = true;
      text.focus();
    });
    form.addEventListener('submit', (e) => {
      e.preventDefault();
      const t = text.value.trim();
      if (!t) { err.textContent = 'Bitte den Schaden beschreiben.'; err.hidden = false; return; }
      if (files.files.length > MAX_PHOTOS) {
        err.textContent = 'Höchstens 10 Fotos.'; err.hidden = false; return;
      }
      result = { text: t, files: [...files.files] };
      dlg.close();
    });
    dlg.addEventListener('close', () => { dlg.remove(); resolve(result); });
    document.body.append(dlg);
    dlg.showModal();
  });
}
