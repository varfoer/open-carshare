# VPS deployment

License: GPL-2.0-only (see LICENSE)

Single Debian/Ubuntu VPS: `ocs-server` on `127.0.0.1:8080` behind Caddy (TLS).
Files here: `ocs-server.service`, `ocs.conf.example`, `Caddyfile`.

## 1. Packages

Build: `build-essential cmake libcurl4-openssl-dev libssl-dev` (cmake >= 3.22,
OpenSSL >= 3.0 for Web Push).
Runtime: `libcurl4`, `libssl3`.

```sh
sudo apt install build-essential cmake libcurl4-openssl-dev libssl-dev git
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

After the first login the admin enters the Verein data (name, address) and
the Gläubiger-ID, creditor IBAN and BIC in „Einstellungen“. Until name and
Gläubiger-ID are set, nobody can give a SEPA mandate. Every member (admins
included) gives a mandate under „Konto“ before booking.

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

If sshd listens on a port other than 22, allow that port before `ufw enable`.

Port 8080 stays closed; the server listens on loopback only.

Outbound HTTPS must stay open (default): mail relay, igloohome and the
browsers' push services (`fcm.googleapis.com`, `*.push.services.mozilla.com`,
`*.push.apple.com`, `*.notify.windows.com`) for push reminders.

Only if `tracker_port` is set in `ocs.conf` (section 10), also open it:

```sh
sudo ufw allow 5027/tcp
```

## 9. Backup

Nightly at 03:15 `ocs-backup.sh` encrypts `data/` with
[age](https://age-encryption.org) and mails it to the admin (spec section 12).
Only `.age` files are written and mailed; the plaintext never touches the disk.

```sh
sudo apt install age
```

On **your own machine** (not the VPS), create the key pair and keep the
private key offline (password manager, USB stick). Without it no backup can be
restored:

```sh
age-keygen -o ocs-backup.key     # prints "Public key: age1..."
```

Put the public `age1...` line into `/etc/ocs/ocs.conf` together with the
recipient address (see `ocs.conf.example`):

```
backup_recipient = age1...
backup_mail_to = admin@example.org
# backup_dir = /var/backups/ocs
# backup_max_mail_bytes = 20000000
```

Install scripts, directory and units:

```sh
sudo install -m 0755 deploy/vps/ocs-backup.sh deploy/vps/ocs-restore.sh /opt/ocs/
sudo install -d -o ocs -g ocs -m 0700 /var/backups/ocs
sudo install -m 0644 deploy/vps/ocs-backup.service deploy/vps/ocs-backup.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ocs-backup.timer
```

Test run, then check that the mail with the `.age` attachment arrived:

```sh
sudo systemctl start ocs-backup.service
journalctl -u ocs-backup.service -n 20
```

Photos are backed up on Sundays (`ocs-photos-*.tar.age`). Mail providers
often cap messages at 20–25 MB: a file over `backup_max_mail_bytes` is not
attached and stays only in `/var/backups/ocs` (30 days); copy it off the VPS by
other means (e.g. `scp`), it is encrypted.

If you used the interim backup cron of earlier versions: remove the line
from `sudo crontab -e` and delete the old unencrypted archives
(`sudo rm /var/backups/ocs/ocs-*.tar.gz`) before the `install -d` above, which
hands the directory to `ocs`.

### Restore

On the machine that holds `ocs-backup.key`, or on the VPS after copying the
key there temporarily:

```sh
sudo systemctl stop ocs-server
sudo /opt/ocs/ocs-restore.sh ocs-data-YYYY-MM-DD.tar.gz.age ocs-backup.key /var/lib/ocs.restored
# photos, if needed:
sudo /opt/ocs/ocs-restore.sh ocs-photos-YYYY-MM-DD.tar.age ocs-backup.key /var/lib/ocs.restored/photos
```

`ocs-restore.sh` refuses a target directory that is not empty. Check the
restored data with a copy of the config whose `data_dir` points at it:

```sh
sudo sh -c "umask 077; sed 's|^data_dir.*|data_dir = /var/lib/ocs.restored|' /etc/ocs/ocs.conf > /root/restore.conf"
sudo /opt/ocs/ocs-server -c /root/restore.conf check-data   # prints table counts, exit 0
sudo rm /root/restore.conf
```

Then swap the directories and start again:

```sh
sudo mv /var/lib/ocs /var/lib/ocs.old && sudo mv /var/lib/ocs.restored /var/lib/ocs
sudo chown -R ocs:ocs /var/lib/ocs
sudo systemctl start ocs-server
```

Delete the key from the VPS again if you copied it there.

## 10. Teltonika tracker

Optional. Set `tracker_port = 5027` in `/etc/ocs/ocs.conf` and restart the
service (`sudo systemctl restart ocs-server`). The server then listens on
`0.0.0.0:<tracker_port>` for Teltonika trackers: plain TCP (no TLS), Codec 8
Extended (`0x8E`) only, no UDP, no Codec 8. Without the key the listener is
disabled. Open the port in the firewall (section 8).

Limits: at most 16 concurrent tracker connections; a connection must send
its IMEI within 30 s; idle connections are closed after 900 s. The handshake
accepts an IMEI only if it is set on a car (active or deactivated). Unknown
IMEIs are logged as `tracker: unknown IMEI <imei>` (`journalctl -u
ocs-server`). Only `car_id`, `ts`, `odometer_m` and `received_at` are stored,
in `odometer_readings.jsonl` in the data directory; readings are pruned
after 90 days.

Settings in the Teltonika Configurator. Option names differ between
firmware versions: check names in your firmware's Configurator.

- GPRS: APN of the SIM, no APN user or password (Simbase: `simbase.eu`, which
  pins the internet breakout to Europe; melita.io via m2m Germany:
  `iot.melita.io`; 1NCE: `iot.1nce.net`). Server
  domain = the VPS host name, port `5027` (the `tracker_port`), protocol
  TCP. Check names in your firmware's Configurator. Simbase is the current
  provider (chosen as the cheapest at the time); the SIM provider may change,
  the tracker settings above are the only thing that depends on it.
- Roaming: Simbase and melita.io SIMs roam on the German networks, so data
  roaming must be allowed (operator list empty or containing the German
  networks). Check names in your firmware's Configurator.
- System / data protocol: Codec 8 Extended. Check names in your firmware's
  Configurator.
- Data acquisition: on moving every 300 s, on stop every 3600 s; records on
  movement start/stop; movement source movement sensor + GNSS (not
  ignition). Check names in your firmware's Configurator.
- Records sorting: oldest first. Check names in your firmware's
  Configurator. The server waits for the trip-end records before it
  resolves km; newest-first sending can deliver the post-return record
  ahead of them.
- I/O: Total Odometer enabled, odometer source GNSS, initial value =
  dashboard km x 1000 (metres). Check names in your firmware's
  Configurator.

Then add the IMEI (device label) in the admin car form.

Check: after the first drive the car's readings appear:

```sh
sudo tail /var/lib/ocs/odometer_readings.jsonl
```

## 11. igloohome key box

Cars with a key box show the member a PIN at trip start. The server asks
igloohome's API for it; this needs an API client of the Verein's igloohome
account.

1. Log in at `web.igloohome.co/api` and create an API client with the
   scopes `algopin-hourly` and `get-devices`. The API is free for 30 days,
   then costs about $2 per box and month; after the trial calls answer 402
   and starts of cars with a box fail with `pin_unavailable`.
2. Put the credentials into `/etc/ocs/ocs.conf`: `igloo_client_id`,
   `igloo_client_secret`. `igloo_api_url` and `igloo_token_url` keep their
   defaults (https only).
3. Restart: `sudo systemctl restart ocs-server`.

Without `igloo_client_id` the server runs normally; only cars with a box
cannot start (log line `igloo not configured`). Setting up a box itself
(pairing, car form, test PIN) is the admin manual on the help page
(`help.html#schluesselbox-einrichten`).

## 12. Billing run and Hibiscus

After a billing run is finalized and sent (admin view „Abrechnung“), the
SEPA collection is imported into Hibiscus by hand:

1. In the run detail click „SEPA-XML herunterladen“ (file
   `run-<id>-pain008.xml`; only present if at least one invoice has a total
   above 0).
2. In Hibiscus: Konto, then „SEPA-Sammellastschrift importieren“, and pick
   the file.
3. Check the collection date, the number of transactions and the total
   against the run, then submit the collection in Hibiscus.

Manual acceptance check of Phase 6 (product owner): „XML imports into
Hibiscus“ without error. It cannot be automated here.

The collection date is fixed at the first send (send date plus
`sepa_lead_days`, next TARGET2 business day) and is the one named in the
invoice mails (pre-notification); a repeated send does not change it.
Invoice PDFs and the XML live under `/var/lib/ocs/invoices/` and
`/var/lib/ocs/billing/`.

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
