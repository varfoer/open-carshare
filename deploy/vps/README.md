# VPS deployment

Single Debian/Ubuntu VPS: `ocs-server` on `127.0.0.1:8080` behind Caddy (TLS).
Files here: `ocs-server.service`, `ocs.conf.example`, `Caddyfile`.

## 1. Packages

Build: `build-essential cmake libcurl4-openssl-dev` (cmake >= 3.22).
Runtime: `libcurl4`.

```sh
sudo apt install build-essential cmake libcurl4-openssl-dev git
```

## 2. Build

```sh
git clone <repo-url> open-carshare && cd open-carshare
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DOCS_SANITIZE=OFF
cmake --build build-rel -j
```

Binary: `build-rel/server/ocs-server`.

## 3. User and directories

```sh
sudo useradd --system --home /var/lib/ocs --shell /usr/sbin/nologin ocs
sudo install -d -o ocs -g ocs -m 0750 /var/lib/ocs
sudo install -d -o root -g ocs -m 0750 /etc/ocs
sudo install -d -m 0755 /opt/ocs /opt/ocs/web
sudo install -m 0755 build-rel/server/ocs-server /opt/ocs/ocs-server
sudo cp -r web/. /opt/ocs/web/ && sudo rm -rf /opt/ocs/web/test
```

## 4. Configuration

```sh
sudo install -m 0640 -o root -g ocs deploy/vps/ocs.conf.example /etc/ocs/ocs.conf
sudoedit /etc/ocs/ocs.conf
```

Set `base_url` to your public URL and the SMTP relay values
(`smtp_url`, `smtp_user`, `smtp_pass`, `mail_from`). TLS to the relay is
mandatory unless it is on localhost. See comments in the file.

## 5. First admin

`create-admin` loads and writes the data directory itself; a running server
does not see the new admin and may overwrite `users.jsonl`. So stop the
service first, run it as user `ocs` (file ownership), then start again.
The password is read from stdin, one line, at least 10 characters.

```sh
sudo systemctl stop ocs-server        # skip on first install
read -rs PW && printf '%s\n' "$PW" | sudo -u ocs /opt/ocs/ocs-server \
  -c /etc/ocs/ocs.conf create-admin admin@example.org "Admin Name"; unset PW
sudo systemctl start ocs-server
```

## 6. systemd

```sh
sudo cp deploy/vps/ocs-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ocs-server
journalctl -u ocs-server -f
```

## 7. Caddy

Install Caddy from the official apt repo:
<https://caddyserver.com/docs/install#debian-ubuntu-raspbian>.
Point a DNS A/AAAA record at the VPS, then:

```sh
sudo cp deploy/vps/Caddyfile /etc/caddy/Caddyfile
echo 'OCS_DOMAIN=carshare.example.org' | sudo tee -a /etc/default/caddy
sudo systemctl edit caddy   # add: [Service] / EnvironmentFile=/etc/default/caddy
sudo systemctl restart caddy
```

(Alternatively replace `{$OCS_DOMAIN}` in the Caddyfile with the domain.)
Caddy sets `X-Forwarded-For`; the server trusts it only from 127.0.0.1.

## 8. Firewall

Only 22, 80 and 443 inbound:

```sh
sudo ufw default deny incoming
sudo ufw allow 22/tcp && sudo ufw allow 80/tcp && sudo ufw allow 443/tcp
sudo ufw enable
```

Port 8080 stays closed; the server listens on loopback only.

## 9. Interim backup

Until mail backups arrive in a later phase, nightly tar via cron
(`sudo crontab -e`):

```cron
0 3 * * * tar -czf /var/backups/ocs-$(date +\%F).tar.gz -C /var/lib ocs && find /var/backups -name 'ocs-*.tar.gz' -mtime +14 -delete
```

Copy the archives off the VPS; the data contains personal data, keep them
private.

## Updating

```sh
git pull
cmake --build build-rel -j
```

For every web change bump `CACHE = 'ocs-v<N>'` in `web/sw.js` before
deploying, or installed PWAs keep serving the old files. Then:

```sh
sudo systemctl stop ocs-server
sudo install -m 0755 build-rel/server/ocs-server /opt/ocs/ocs-server
sudo rm -rf /opt/ocs/web && sudo install -d /opt/ocs/web \
  && sudo cp -r web/. /opt/ocs/web/ && sudo rm -rf /opt/ocs/web/test
sudo systemctl start ocs-server
```
