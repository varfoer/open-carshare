import { api } from '../api.js';
import { formatDateTime } from '../time.js';
import { el } from '../admin_ui.js';
import { priceSummary } from '../price.js';

function kmText(b) {
  if (b.km_pending) return 'km ausstehend';
  if (b.km_source === 'none') return '';
  return `${b.km_start} – ${b.km_end} km (${b.km_end - b.km_start} km)`;
}

function tripText(b) {
  if (b.state === 'cancelled') return 'Storniert';
  if (!b.actual_start || !b.actual_end) return 'Nicht angetreten';
  return `Gefahren: ${formatDateTime(b.actual_start)} – ${formatDateTime(b.actual_end)}`;
}

export async function render(root) {
  const [list, cars] = await Promise.all([api('GET', '/api/bookings?scope=history'), api('GET', '/api/cars')]);
  const names = new Map(cars.map((c) => [c.id, c.name]));
  root.append(el('h1', { text: 'Verlauf' }));
  if (!list.length) {
    root.append(el('p', { className: 'placeholder', text: 'Noch keine abgeschlossenen Buchungen.' }));
    return;
  }
  for (const b of list) {
    const car = el('p', { className: 'booking-car', text: names.get(b.car_id) || '' });
    if (b.club_trip) car.append(' ', el('span', { className: 'badge', text: 'Vereinsfahrt' }));
    const card = el('div', { className: 'card booking' },
      el('p', { className: 'booking-time', text: `${formatDateTime(b.booked_start)} – ${formatDateTime(b.booked_end)}` }),
      car,
      el('p', { text: tripText(b) }));
    const km = b.state === 'cancelled' ? '' : kmText(b);
    if (km) card.append(el('p', { text: km }));
    if (b.price && (b.state === 'done' || b.state === 'cancelled')) card.append(el('p', { text: `Preis: ${priceSummary(b.price)}` }));
    root.append(card);
  }
}
