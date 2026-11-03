import { api } from '../api.js';
import { el, adminNav, messageBox, selectField } from '../admin_ui.js';
import { formatEuro } from '../money.js';
import { formatDateTime } from '../time.js';
import { lastMonth, periodLabel, dateDe, blockerLines } from '../billing.js';

const stateLabel = (r) => (r.state === 'draft' ? 'Entwurf' : r.sent_at ? 'Versendet' : 'Abgeschlossen');

// Last 24 closed months, newest first, default = previous month.
function periodOptions() {
  const out = [];
  let [y, m] = lastMonth(new Date()).split('-').map(Number);
  for (let i = 0; i < 24; i++) {
    const p = `${y}-${String(m).padStart(2, '0')}`;
    out.push([p, periodLabel(p)]);
    if (--m === 0) { m = 12; y--; }
  }
  return out;
}

function button(text, onClick, cls) {
  const b = el('button', { type: 'button', text, ...(cls ? { className: cls } : {}) });
  b.addEventListener('click', onClick);
  return b;
}

function lineText(l) {
  const parts = [`Buchung ${l.booking_id}`, l.car_name, formatDateTime(l.start)];
  if (l.club_trip) parts.push('Vereinsfahrt');
  if (l.cancelled) parts.push('storniert');
  if (l.km) parts.push(`${l.km} km`);
  return `${parts.join(' · ')}: ${formatEuro(l.total_cents)}`;
}

export async function render(root, params = {}) {
  const err = messageBox('error');
  const ok = messageBox('success');
  const host = el('div');
  root.append(adminNav('#/admin/abrechnung'), el('h1', { text: 'Abrechnung' }), err.node, ok.node, host);
  const runId = Number(params.id) || 0;
  if (runId) await showDetail(runId); else await showList();

  function fail(x) { ok.hide(); err.show(x.message); }
  function open(id) { location.hash = id ? `#/admin/abrechnung?id=${id}` : '#/admin/abrechnung'; }

  async function showList() {
    err.hide();
    const periods = selectField('Zeitraum', periodOptions(), lastMonth(new Date()));
    const create = button('Entwurf erstellen', async () => {
      err.hide(); ok.hide();
      try {
        const r = await api('POST', `/api/admin/billing?period=${periods.input.value}`);
        open(r.id);
      } catch (x) { fail(x); }
    }, 'primary');
    const list = el('div');
    host.replaceChildren(el('div', { className: 'card' }, el('h2', { text: 'Neue Abrechnung' }), periods.node, create), list);
    let runs;
    try { runs = await api('GET', '/api/admin/billing'); } catch (x) { return fail(x); }
    if (!runs.length) list.append(el('p', { className: 'placeholder', text: 'Noch keine Abrechnungen.' }));
    for (const r of runs) {
      const total = r.state === 'final' ? formatEuro(r.total_cents) : '–';
      const a = el('a', { href: `#/admin/abrechnung?id=${r.id}`, text: periodLabel(r.period) });
      list.append(el('div', { className: 'card booking' },
        el('p', { className: 'booking-time' }, a),
        el('p', { className: 'booking-car', text: `${stateLabel(r)} · ${total}` })));
    }
  }

  async function showDetail(id, keepMessages = false) {
    if (!keepMessages) { err.hide(); ok.hide(); }
    let d, settings;
    try {
      [d, settings] = await Promise.all([api('GET', `/api/admin/billing/${id}`), api('GET', '/api/admin/settings')]);
    } catch (x) { fail(x); return host.replaceChildren(backLink()); }
    const run = d.run;
    const usersById = new Map(d.users.map((u) => [u.user_id, u]));
    const head = el('div', { className: 'card' },
      el('h2', { text: `${periodLabel(run.period)} · ${stateLabel(run)}` }),
      el('p', { text: `Summe: ${formatEuro(d.total_cents)} · ${d.users.length} Rechnungen` }));
    if (run.collection_date) head.append(el('p', { text: `Einzug am ${dateDe(run.collection_date)}` }));
    const parts = [backLink(), head];

    const lines = blockerLines(d.blockers, usersById);
    if (lines.length) {
      const ul = el('ul');
      for (const t of lines) ul.append(el('li', { text: t }));
      parts.push(el('div', { className: 'card' }, el('h2', { text: 'Abschluss blockiert' }), ul,
        el('p', {}, el('a', { href: '#/admin/buchungen', text: 'Zu den Buchungen' }))));
    }

    parts.push(usersTable(d));

    const actions = el('div', { className: 'dialog-actions' });
    if (run.state === 'draft') {
      actions.append(
        button('Neu berechnen', () => showDetail(id)),
        button('Löschen', async () => {
          if (!window.confirm(`Entwurf ${periodLabel(run.period)} löschen?`)) return;
          try { await api('DELETE', `/api/admin/billing/${id}`); open(0); } catch (x) { fail(x); }
        }, 'danger'),
        button('Abschließen', async () => {
          if (!window.confirm(`${d.users.length} Rechnungen über ${formatEuro(d.total_cents)} abschließen? Das lässt sich nicht rückgängig machen.`)) return;
          try {
            await api('POST', `/api/admin/billing/${id}/finalize`);
            await showDetail(id);
          } catch (x) {
            if (x.code === 'blocked') await showDetail(id, true);
            fail(x);
          }
        }, 'primary'));
    } else {
      const unsent = d.users.some((u) => u.invoice && !u.invoice.mailed_at);
      if (unsent) {
        actions.append(button(run.sent_at ? 'Erneut versenden' : 'Versenden', async () => {
          if (!window.confirm(`Rechnungen jetzt per Mail versenden? Der Einzug erfolgt frühestens in ${settings.sepa_lead_days} Tagen.`)) return;
          try {
            const r = await api('POST', `/api/admin/billing/${id}/send`);
            await showDetail(id, true);
            if (r.failed.length) fail({ message: `${r.sent} versendet, nicht zugestellt: Rechnung ${r.failed.join(', ')}.` });
            else ok.show(`${r.sent} Rechnungen versendet.`);
          } catch (x) { fail(x); }
        }, 'primary'));
      }
      if (run.sepa_file) {
        actions.append(el('a', { className: 'btn', href: `/api/admin/billing/${id}/sepa.xml`, download: `lastschrift-${run.period}.xml`, text: 'SEPA-XML herunterladen' }));
      }
    }
    parts.push(actions);
    host.replaceChildren(...parts);

    function usersTable(data) {
      const table = el('table', { className: 'data' });
      table.append(el('thead', {}, el('tr', {}, ...['Nutzer', 'Mandat', 'Summe', 'Rechnung'].map((t) => el('th', { text: t })))));
      const body = el('tbody');
      for (const u of data.users) {
        const inv = u.invoice;
        const invCell = el('td');
        if (inv) {
          invCell.append(el('a', { href: `/api/admin/invoices/${inv.number}/pdf`, target: '_blank', rel: 'noopener', text: `PDF ${inv.number}` }),
            ` · ${inv.mailed_at ? `versendet ${formatDateTime(inv.mailed_at)}` : 'nicht versendet'}`);
        }
        const det = el('details', {}, el('summary', { text: u.name }));
        const ul = el('ul');
        for (const l of u.lines) ul.append(el('li', { text: lineText(l) }));
        det.append(ul);
        body.append(el('tr', {}, el('td', {}, det),
          el('td', { text: u.mandate ? `${u.mandate.ref} (${u.mandate.iban_masked})` : 'kein Mandat' }),
          el('td', { text: formatEuro(u.total_cents) }), invCell));
      }
      table.append(body);
      return el('div', { className: 'table-wrap' }, table);
    }
  }

  function backLink() { return el('p', {}, el('a', { href: '#/admin/abrechnung', text: '‹ Alle Abrechnungen' })); }
}
