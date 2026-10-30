// JSON API client. Every non-GET request sends Content-Type: application/json
// (the server answers 415 otherwise), even without a body.
export class ApiError extends Error {
  constructor(status, code, message) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.code = code;
  }
}

let unauthorizedHandler = null;
export function onUnauthorized(fn) { unauthorizedHandler = fn; }

export async function api(method, path, body) {
  const opts = { method, credentials: 'same-origin', headers: {} };
  if (method !== 'GET') {
    opts.headers['Content-Type'] = 'application/json';
    if (body !== undefined) opts.body = JSON.stringify(body);
  }
  let res;
  try {
    res = await fetch(path, opts);
  } catch {
    throw new ApiError(0, 'network', 'Keine Verbindung zum Server.');
  }
  let data = null;
  const text = await res.text();
  if (text) {
    try { data = JSON.parse(text); } catch { data = null; }
  }
  if (!res.ok) {
    const code = (data && data.error) || 'error';
    const message = (data && data.message) || 'Es ist ein Fehler aufgetreten.';
    if (res.status === 401 && code === 'unauthenticated' && unauthorizedHandler) unauthorizedHandler();
    throw new ApiError(res.status, code, message);
  }
  return data;
}
