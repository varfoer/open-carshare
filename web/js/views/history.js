import { api } from '../api.js';
import { formatDateTime } from '../time.js';
import { el } from '../admin_ui.js';
import { priceSummary } from '../price.js';
import { bookingCard } from '../booking_card.js';

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
  const now = Math.floor(Date.now() / 1000);
  root.append(el('h1', { text: 'Verlauf' }));
  if (!list.length) {
    root.append(el('p', { className: 'placeholder', text: 'Noch keine abgeschlossenen Buchungen.' }));
    return;
  }
  for (const b of list) {
    const badges = [];
    if (b.state === 'cancelled') badges.push({ text: 'Storniert', kind: 'warn' });
    if (b.club_trip) badges.push({ text: 'Vereinsfahrt' });
    const lines = [];
    if (cars.length > 1 && names.get(b.car_id)) lines.push(names.get(b.car_id));
    if (b.state !== 'cancelled') lines.push(tripText(b));
    const km = b.state === 'cancelled' ? '' : kmText(b);
    if (km) lines.push(km);
    if (b.price && (b.state === 'done' || b.state === 'cancelled')) {
      lines.push(el('p', { className: 'booking-time', text: priceSummary(b.price) }));
    }
    root.append(bookingCard({ start: b.booked_start, end: b.booked_end, nowSec: now, badges, lines }));
  }
}
