// IBAN helpers. Client-side plausibility only; the server stays authoritative
// (country length table).
export function normalizeIban(s) {
  return String(s).replace(/\s+/g, '').toUpperCase();
}

export function ibanValid(s) {
  const n = normalizeIban(s);
  if (!/^[A-Z]{2}\d{2}[A-Z0-9]{11,30}$/.test(n)) return false;
  const r = n.slice(4) + n.slice(0, 4);
  let rem = 0;
  for (const c of r) {
    const v = c >= 'A' ? String(c.charCodeAt(0) - 55) : c;
    for (const d of v) rem = (rem * 10 + (d.charCodeAt(0) - 48)) % 97;
  }
  return rem === 1;
}

export function formatIban(s) {
  return normalizeIban(s).replace(/(.{4})(?=.)/g, '$1 ');
}
