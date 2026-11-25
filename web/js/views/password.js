import { api } from '../api.js';
import { refreshUser, navigate } from '../app.js';
import { setLoginNotice } from './login.js';

export function render(root, params) {
  if (params && params.token) renderSet(root, params.token);
  else renderForgot(root);
}

function bind(form, onSubmit) {
  const err = form.querySelector('.error');
  const ok = form.querySelector('.success');
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    err.hidden = true;
    ok.hidden = true;
    const btn = form.querySelector('button.primary');
    btn.disabled = true;
    try {
      const msg = await onSubmit(form);
      if (msg) { ok.textContent = msg; ok.hidden = false; }
    } catch (ex) {
      err.textContent = ex.message;
      err.hidden = false;
      btn.disabled = false;
    }
  });
}

function renderForgot(root) {
  root.innerHTML = `
    <form class="card">
      <h1>Passwort vergessen</h1>
      <p>Wir schicken dir einen Link zum Zurücksetzen.</p>
      <label>E-Mail <input name="email" type="email" autocomplete="username" required></label>
      <p class="error" role="alert" hidden></p>
      <p class="success" role="status" hidden></p>
      <button class="primary" type="submit">Link senden</button>
      <p><a href="#/login">Zurück zur Anmeldung</a></p>
    </form>`;
  bind(root.querySelector('form'), async (f) => {
    await api('POST', '/api/password/forgot', { email: f.email.value.trim() });
    return 'Falls die Adresse bekannt ist, wurde ein Link verschickt.';
  });
}

function renderSet(root, token) {
  root.innerHTML = `
    <form class="card">
      <h1>Neues Passwort</h1>
      <label>Neues Passwort (mindestens 10 Zeichen) <input name="pw1" type="password" autocomplete="new-password" minlength="10" required></label>
      <label>Passwort wiederholen <input name="pw2" type="password" autocomplete="new-password" minlength="10" required></label>
      <p class="error" role="alert" hidden></p>
      <p class="success" role="status" hidden></p>
      <button class="primary" type="submit">Passwort speichern</button>
      <p><a href="#/login">Zurück zur Anmeldung</a></p>
    </form>`;
  bind(root.querySelector('form'), async (f) => {
    if (f.pw1.value !== f.pw2.value) throw new Error('Die Passwörter stimmen nicht überein.');
    await api('POST', '/api/password/set', { token, password: f.pw1.value });
    // The server ended all sessions of this user; drop a stale login before switching.
    setLoginNotice('Passwort gespeichert. Du kannst dich jetzt anmelden.');
    await refreshUser();
    navigate('#/login');
    return '';
  });
}
