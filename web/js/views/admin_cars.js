import { api } from '../api.js';
import { el, adminNav, messageBox, textField, checkField, primaryButton } from '../admin_ui.js';

export async function render(root) {
  const cars = await api('GET', '/api/admin/cars');
  const err = messageBox('error');
  const list = el('div');
  const formHost = el('div');
  root.append(adminNav('#/admin/autos'), el('h1', { text: 'Autos' }), err.node, list, formHost);

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
    const active = checkField('aktiv', car ? car.active : true);
    const cancel = el('button', { type: 'button', text: 'Abbrechen', className: 'btn' });
    cancel.addEventListener('click', () => formHost.replaceChildren());
    const form = el('form', { className: 'card' },
      el('h2', { text: car ? 'Auto bearbeiten' : 'Auto anlegen' }),
      name.node, plate.node, loc.node, active.node, primaryButton('Speichern'), cancel);
    form.addEventListener('submit', (ev) => {
      ev.preventDefault();
      save(car, {
        name: name.input.value.trim(), plate: plate.input.value.trim(),
        location: loc.input.value.trim(), active: active.input.checked,
      });
    });
    formHost.replaceChildren(form);
    form.scrollIntoView();
  }

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
      el('p', { className: 'booking-car', text: `${c.location || 'ohne Standort'} · ${c.active ? 'aktiv' : 'inaktiv'}` }),
      actions));
  }
  if (!cars.length) list.append(el('p', { className: 'placeholder', text: 'Keine Autos.' }));
  const add = el('button', { type: 'button', className: 'primary', text: 'Auto anlegen' });
  add.addEventListener('click', () => showForm(null));
  list.after(add);
}
