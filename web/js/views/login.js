import { api } from '../api.js';
import { refreshUser, navigate } from '../app.js';

// Shown once above the next login form, e.g. after a new password was set.
let notice = '';
export function setLoginNotice(text) { notice = text; }

export function render(root) {
  root.innerHTML = `
    <picture>
      <source srcset="img/logo-full-dark.svg" media="(prefers-color-scheme: dark)">
      <img class="login-logo" src="img/logo-full.svg" alt="Dörpsmobil SH" width="260" height="75">
    </picture>
    <form class="card" id="login-form">
      <h1>Anmelden</h1>
      <p class="success" role="status" hidden></p>
      <label>E-Mail <input name="email" type="email" autocomplete="username" required></label>
      <label>Passwort <input name="password" type="password" autocomplete="current-password" required></label>
      <p class="error" role="alert" hidden></p>
      <button class="primary" type="submit">Anmelden</button>
      <p><a href="#/passwort">Passwort vergessen?</a> · <a href="/help.html">Hilfe</a></p>
    </form>`;
  const form = root.querySelector('form');
  const err = root.querySelector('.error');
  const ok = root.querySelector('.success');
  ok.textContent = notice;
  ok.hidden = !notice;
  notice = '';
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    err.hidden = true;
    ok.hidden = true;
    const btn = form.querySelector('button');
    btn.disabled = true;
    try {
      await api('POST', '/api/login', {
        email: form.email.value.trim(),
        password: form.password.value,
      });
      await refreshUser();
      navigate('#/kalender');
    } catch (ex) {
      err.textContent = ex.message;
      err.hidden = false;
      btn.disabled = false;
    }
  });
}
