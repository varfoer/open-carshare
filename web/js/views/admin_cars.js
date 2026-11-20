import { api } from '../api.js';
import { formatTime } from '../time.js';
import { el, adminPage, pageHead, messageBox, textField, checkField, primaryButton } from '../admin_ui.js';

export async function render(root) {
  const cars = await api('GET', '/api/admin/cars');
  const err = messageBox('error');
  const list = el('div');
  const formHost = el('div');
  const add = el('button', { type: 'button', className: 'primary', text: 'Auto anlegen' });
  const main = adminPage(root, '#/admin/autos');
  main.append(pageHead('Autos', '', add), err.node, list, formHost);

  const reload = () => { root.replaceChildren(); return render(root); };

  async function save(car, body) {
    err.hide();
    try {
      if (car) await api('PUT', `/api/admin/cars/${car.id}`, body);
      else await api('POST', '/api/admin/cars', body);
      await reload();
    } catch (x) { err.show(x.message); }
  }

  function showForm(car) {
    const name = textField('Name', car ? car.name : '', { required: true });
    const plate = textField('Kennzeichen', car ? car.plate : '', { required: true });
    const loc = textField('Standort', car ? car.location : '');
    const imei = textField('Tracker-IMEI', car ? car.tracker_imei : '', { inputmode: 'numeric', pattern: '\\d{15}' });
    const active = checkField('aktiv', car ? car.active : true);
    const saved = car ? car.lock_device_id || '' : '';
    const lockHost = el('label', {}, 'Schlüsselbox ', el('span', { className: 'hint', text: 'lädt …' }));
    let lockInput = null;
    const lockValue = () => (lockInput ? lockInput.value.trim() : saved);
    const testBtn = el('button', { type: 'button', text: 'Box prüfen' });
    const testOut = el('p', { className: 'hint' });
    function syncTest() {
      const dirty = lockInput !== null && lockValue() !== saved;
      testBtn.disabled = dirty;
      testBtn.title = dirty ? 'Erst speichern' : '';
      testBtn.textContent = dirty ? 'Erst speichern' : 'Box prüfen';
    }
    testBtn.addEventListener('click', async () => {
      testBtn.disabled = true;
      testOut.textContent = 'Test-PIN wird angefordert …';
      try {
        const r = await api('POST', `/api/admin/cars/${car.id}/lock-test`);
        testOut.textContent = `Test-PIN ${r.pin}, gültig bis ${formatTime(r.valid_until)} — an der Box eingeben. Öffnet sie, ist alles eingerichtet.`;
      } catch (x) { testOut.textContent = x.message; }
      syncTest();
    });
    api('GET', '/api/admin/locks').then((locks) => {
      const sel = el('select');
      sel.append(el('option', { value: '', text: 'Keine' }));
      for (const l of locks) sel.append(el('option', { value: l.device_id, text: l.name || l.device_id }));
      if (saved && !locks.some((l) => l.device_id === saved)) sel.append(el('option', { value: saved, text: `Unbekannte Box (${saved})` }));
      sel.value = saved;
      lockInput = sel;
      lockHost.replaceChildren('Schlüsselbox ', sel);
    }).catch((x) => {
      const t = el('input', { type: 'text', value: saved });
      lockInput = t;
      lockHost.replaceChildren('Schlüsselbox ', t, el('span', { className: 'hint', text: x.message }));
    }).finally(() => {
      lockInput.addEventListener('input', syncTest);
      lockInput.addEventListener('change', syncTest);
      syncTest();
    });
    const help = el('a', { href: 'help.html#schluesselbox-einrichten', target: '_blank', rel: 'noopener', text: 'Anleitung' });
    const lockBlock = el('div', {}, lockHost, help);
    if (car && saved) lockBlock.append(el('div', { className: 'dialog-actions' }, testBtn), testOut);
    const cancel = el('button', { type: 'button', text: 'Abbrechen', className: 'btn' });
    cancel.addEventListener('click', () => formHost.replaceChildren());
    const form = el('form', { className: 'card' },
      el('h2', { text: car ? 'Auto bearbeiten' : 'Auto anlegen' }),
      name.node, plate.node, loc.node, imei.node, lockBlock, active.node, primaryButton('Speichern'), cancel);
    form.addEventListener('submit', (ev) => {
      ev.preventDefault();
      const lv = lockInput ? lockValue() : undefined;
      save(car, {
        ...(lv !== undefined && { lock_device_id: lv }),
        name: name.input.value.trim(), plate: plate.input.value.trim(),
        location: loc.input.value.trim(), tracker_imei: imei.input.value.trim(), active: active.input.checked,
      });
    });
    formHost.replaceChildren(form);
    form.scrollIntoView();
  }

  const lockNames = new Map();
  try { for (const l of await api('GET', '/api/admin/locks')) if (l.name) lockNames.set(l.device_id, l.name); } catch { /* ids shown */ }

  for (const c of cars) {
    const edit = el('button', { type: 'button', text: 'Bearbeiten' });
    edit.addEventListener('click', () => showForm(c));
    const actions = el('div', { className: 'dialog-actions' }, edit);
    if (c.active) {
      const off = el('button', { type: 'button', className: 'danger', text: 'Deaktivieren' });
      off.addEventListener('click', () => save(c, { name: c.name, plate: c.plate, location: c.location, active: false }));
      actions.append(off);
    }
    list.append(el('div', { className: 'card booking' },
      el('p', { className: 'booking-time', text: `${c.name} (${c.plate})` }),
      el('p', { className: 'booking-car', text: `${c.location || 'ohne Standort'} · ${c.tracker_imei ? `Tracker ${c.tracker_imei}` : 'ohne Tracker'} · ${c.lock_device_id ? `Box ${lockNames.get(c.lock_device_id) || c.lock_device_id}` : 'ohne Box'} · ${c.active ? 'aktiv' : 'inaktiv'}` }),
      actions));
  }
  if (!cars.length) list.append(el('p', { className: 'placeholder', text: 'Keine Autos.' }));
  add.addEventListener('click', () => showForm(null));
}
