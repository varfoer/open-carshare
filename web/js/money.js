// Euro <-> integer cents, string parsing only (no float multiplication).
export function formatEuro(cents) {
  const abs = Math.abs(cents);
  return `${cents < 0 ? '-' : ''}${Math.floor(abs / 100)},${String(abs % 100).padStart(2, '0')} €`;
}

// "5", "5,00", "5.5" -> cents; null when invalid. Max 2 decimals, no sign, no thousands separators.
export function parseEuroToCents(text) {
  const m = /^(\d{1,12})(?:[.,](\d{1,2}))?$/.exec(String(text).trim());
  if (!m) return null;
  return Number(m[1]) * 100 + Number((m[2] || '').padEnd(2, '0') || 0);
}
