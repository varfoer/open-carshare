import { api } from '../api.js';
import { navigate } from '../app.js';
import { berlinToEpoch, epochToBerlin, formatTime, startOfBerlinDay, sameBerlinDay } from '../time.js';
import { icon } from '../icons.js';

const DAY = 86400;
const WD_DE = ['', 'Mo', 'Di', 'Mi', 'Do', 'Fr', 'Sa', 'So'];
const p2 = (n) => String(n).padStart(2, '0');

// Selected car and anchor survive navigation inside the session.
let selectedCar = null;
let anchor = null;

function mondayOf(ts) {
  const b = epochToBerlin(ts);
  return startOfBerlinDay(startOfBerlinDay(ts) - (b.wd - 1) * DAY + DAY / 2);
}

// Day starts (epoch) for n consecutive Berlin days from `first`; the 12 h
// offset keeps the lookup inside the right day on 23/25 h days.
function dayStarts(first, n) {
  const out = [first];
  for (let i = 1; i <= n; i++) out.push(startOfBerlinDay(out[i - 1] + DAY * 1.5));
  return out;
}

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

export async function render(root) {
  root.className = 'view wide';
  const cars = await api('GET', '/api/cars');
  if (!cars.length) {
    root.append(el('p', 'placeholder', 'Kein Fahrzeug verfügbar.'));
    return;
  }
  if (!cars.some((c) => c.id === selectedCar)) selectedCar = cars[0].id;
  const nowTs = Math.floor(Date.now() / 1000);
  if (anchor === null) anchor = nowTs;

  let weekMode = window.innerWidth >= 700;
  const bar = el('div', 'cal-bar');
  const body = el('div', 'cal-body');
  const err = el('p', 'error');
  err.hidden = true;
  err.setAttribute('role', 'alert');
  root.append(bar, err, body);

  const onResize = () => {
    if (!root.isConnected) { window.removeEventListener('resize', onResize); return; }
    const w = window.innerWidth >= 700;
    if (w !== weekMode) { weekMode = w; draw(); }
  };
  window.addEventListener('resize', onResize);

  function shift(dir) {
    if (weekMode) {
      anchor = mondayOf(dir > 0 ? mondayOf(anchor) + 8 * DAY : mondayOf(anchor) - DAY / 2);
    } else {
      anchor = dir > 0 ? startOfBerlinDay(startOfBerlinDay(anchor) + 1.5 * DAY)
        : startOfBerlinDay(startOfBerlinDay(anchor) - DAY / 2);
    }
    draw();
  }

  function drawBar(days) {
    bar.replaceChildren();
    if (cars.length > 1) {
      const sel = el('select');
      sel.setAttribute('aria-label', 'Fahrzeug');
      for (const c of cars) {
        const o = el('option', '', `${c.name} (${c.plate})`);
        o.value = c.id;
        o.selected = c.id === selectedCar;
        sel.append(o);
      }
      sel.addEventListener('change', () => { selectedCar = Number(sel.value); draw(); });
      bar.append(sel);
    }
    const nav = el('div', 'cal-nav');
    const prev = el('button', 'cal-btn', '‹');
    prev.type = 'button';
    prev.setAttribute('aria-label', 'Zurück');
    prev.addEventListener('click', () => shift(-1));
    const today = el('button', 'cal-btn today', 'Heute');
    today.type = 'button';
    today.addEventListener('click', () => { anchor = Math.floor(Date.now() / 1000); draw(); });
    const next = el('button', 'cal-btn', '›');
    next.type = 'button';
    next.setAttribute('aria-label', 'Weiter');
    next.addEventListener('click', () => shift(1));
    const a = epochToBerlin(days[0]);
    const z = epochToBerlin(days[days.length - 1]);
    const title = el('span', 'cal-title', weekMode
      ? `${p2(a.d)}.${p2(a.mo)}. – ${p2(z.d)}.${p2(z.mo)}.${z.y}`
      : `${WD_DE[a.wd]}, ${p2(a.d)}.${p2(a.mo)}.${a.y}`);
    nav.append(prev, today, next, title);
    bar.append(nav);
  }

  let drawSeq = 0;
  let firstDraw = true;
  async function draw() {
    const seq = ++drawSeq;
    const first = weekMode ? mondayOf(anchor) : startOfBerlinDay(anchor);
    const n = weekMode ? 7 : 1;
    const starts = dayStarts(first, n);
    const days = starts.slice(0, n);
    drawBar(days);
    err.hidden = true;
    let busy;
    try {
      busy = await api('GET', `/api/calendar?car=${selectedCar}&from=${starts[0]}&to=${starts[n]}`);
    } catch (e) {
      if (seq !== drawSeq) return;
      err.textContent = e.message;
      err.hidden = false;
      body.replaceChildren();
      return;
    }
    if (seq !== drawSeq) return;
    const slot = Math.ceil(Math.floor(Date.now() / 1000) / 900) * 900;
    const cta = el('a', 'btn primary cal-cta', 'Buchen');
    cta.href = `#/buchen?car=${selectedCar}&start=${slot}&end=${slot + 7200}`;
    cta.prepend(icon('plus'));
    body.replaceChildren(grid(days, starts[n], busy), cta);
    // First paint: bring the current hour into view instead of starting at midnight.
    if (firstDraw) {
      firstDraw = false;
      const cell = body.querySelector('.cal-cell.now');
      if (cell) cell.scrollIntoView({ block: 'center' });
    }
  }

  function grid(days, endTs, busy) {
    const g = el('div', 'cal-grid');
    g.style.gridTemplateColumns = `3rem repeat(${days.length}, minmax(0, 1fr))`;
    const now = Math.floor(Date.now() / 1000);
    g.append(el('div', 'cal-corner'));
    for (const d of days) {
      const b = epochToBerlin(d);
      g.append(el('div', sameBerlinDay(d, now) ? 'cal-head today' : 'cal-head', `${WD_DE[b.wd]} ${p2(b.d)}.${p2(b.mo)}.`));
    }
    const shown = new Set();
    for (let r = 0; r < 48; r++) {
      const h = r >> 1, mi = (r & 1) * 30;
      const lab = el('div', 'cal-time', mi === 0 ? `${p2(h)}:00` : '');
      g.append(lab);
      for (const d of days) {
        const b = epochToBerlin(d);
        const s = berlinToEpoch(b.y, b.mo, b.d, h, mi);
        const sb = epochToBerlin(s);
        const exists = sb.d === b.d && sb.h === h && sb.mi === mi;
        const nr = r + 1;
        const e = nr === 48 ? startOfBerlinDay(d + 1.5 * DAY)
          : berlinToEpoch(b.y, b.mo, b.d, nr >> 1, (nr & 1) * 30);
        if (!exists) { g.append(el('div', 'cal-cell gap')); continue; }
        const isNow = s <= now && now < e;
        const hit = busy.find((x) => x.start < e && x.end > s);
        if (hit) {
          const cell = el(hit.own ? 'button' : 'div', `cal-cell ${hit.own ? 'own' : 'other'}${isNow ? ' now' : ''}`);
          const key = `${hit.start}-${hit.end}-${d}`;
          if (!shown.has(key) && (hit.start >= s || r === 0)) {
            shown.add(key);
            cell.textContent = hit.own ? `${formatTime(hit.start)}–${formatTime(hit.end)}` : 'belegt';
          }
          if (hit.own) {
            cell.type = 'button';
            cell.setAttribute('aria-label', 'Eigene Buchung verlängern');
            cell.addEventListener('click', () => navigate(`#/buchen?id=${hit.id}`));
          }
          g.append(cell);
          continue;
        }
        const cell = el('button', isNow ? 'cal-cell free now' : 'cal-cell free');
        cell.type = 'button';
        cell.setAttribute('aria-label', `Frei ${p2(b.d)}.${p2(b.mo)}. ${p2(h)}:${p2(mi)}`);
        if (s < now) cell.disabled = true;
        else cell.addEventListener('click', () =>
          navigate(`#/buchen?car=${selectedCar}&start=${s}&end=${s + 7200}`));
        g.append(cell);
      }
    }
    return g;
  }

  await draw();
}
