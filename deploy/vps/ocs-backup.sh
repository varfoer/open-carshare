#!/usr/bin/env bash
# Nightly encrypted backup of the open-carshare data dir, mailed to the admin.
#
# usage: ocs-backup.sh [-c /etc/ocs/ocs.conf] [--photos]
#
# Reads data_dir, smtp_url, smtp_user, smtp_pass, mail_from and the backup_* keys from the
# server config (same `key = value` / `#` syntax). Writes, encrypted with age to
# backup_recipient, into backup_dir:
#   ocs-data-YYYY-MM-DD.tar.gz.age    data_dir without photos/ and *.tmp (daily)
#   ocs-photos-YYYY-MM-DD.tar.age     photos/ (Sundays, or with --photos)
# deletes ocs-*.age older than 30 days there, and mails the new files (each attached only up
# to backup_max_mail_bytes) to backup_mail_to via smtp_url.
# Plaintext never touches the disk: tar is piped straight into age.
#
# Exit 0 ok, 2 usage/config error, 1 any other failure (local files stay).
# Test hook: OCS_BACKUP_MAIL_FILE=<path> writes the message there instead of sending it.
set -euo pipefail
umask 077

CONF=/etc/ocs/ocs.conf
PHOTOS=0
while [ $# -gt 0 ]; do
  case "$1" in
    -c) [ $# -ge 2 ] || { echo "ocs-backup: -c needs a file" >&2; exit 2; }; CONF=$2; shift 2 ;;
    --photos) PHOTOS=1; shift ;;
    *) echo "usage: ocs-backup.sh [-c /etc/ocs/ocs.conf] [--photos]" >&2; exit 2 ;;
  esac
done
[ -r "$CONF" ] || { echo "ocs-backup: cannot read $CONF" >&2; exit 2; }

data_dir='' smtp_url='' smtp_user='' smtp_pass='' mail_from=''
backup_recipient='' backup_mail_to='' backup_dir=/var/backups/ocs backup_max_mail_bytes=20000000
while IFS= read -r line || [ -n "$line" ]; do
  line=${line%$'\r'}
  line="${line#"${line%%[![:space:]]*}"}"
  [ -z "$line" ] || [ "${line:0:1}" = '#' ] && continue
  [[ "$line" == *=* ]] || continue
  key=${line%%=*} val=${line#*=}
  key="${key%"${key##*[![:space:]]}"}"
  val="${val#"${val%%[![:space:]]*}"}"
  val="${val%"${val##*[![:space:]]}"}"
  case "$key" in
    data_dir|smtp_url|smtp_user|smtp_pass|mail_from|backup_recipient|backup_mail_to|backup_dir|backup_max_mail_bytes)
      printf -v "$key" '%s' "$val" ;;
  esac
done < "$CONF"

[ -n "$backup_recipient" ] || { echo "ocs-backup: backup_recipient missing in $CONF" >&2; exit 2; }
[ -n "$backup_mail_to" ] || { echo "ocs-backup: backup_mail_to missing in $CONF" >&2; exit 2; }
[ -n "$data_dir" ] && [ -d "$data_dir" ] || { echo "ocs-backup: data_dir missing or not a directory" >&2; exit 2; }
[[ "$backup_max_mail_bytes" =~ ^[0-9]+$ ]] || { echo "ocs-backup: backup_max_mail_bytes not an integer" >&2; exit 2; }
command -v age >/dev/null || { echo "ocs-backup: age not installed" >&2; exit 1; }

mkdir -p "$backup_dir"
DATE=$(date +%F)
STATUS=0
NEW=()

# encrypt <name> <tar options...>: tar of data_dir piped into age, written as backup_dir/<name>.
# GNU tar exit 1 ("file changed as we read it") counts as success.
encrypt() {
  local name=$1 part="$backup_dir/.$1.part"
  shift
  set +e
  tar "$@" | age -r "$backup_recipient" -o "$part"
  local st=("${PIPESTATUS[@]}")
  set -e
  if [ "${st[0]}" -gt 1 ] || [ "${st[1]}" -ne 0 ]; then
    echo "ocs-backup: $name failed (tar ${st[0]}, age ${st[1]})" >&2
    rm -f "$part"
    STATUS=1
    return 0
  fi
  mv -f "$part" "$backup_dir/$name"
  NEW+=("$name")
}

encrypt "ocs-data-$DATE.tar.gz.age" -C "$data_dir" --exclude=./photos --exclude='*.tmp' -czf - .
if [ -d "$data_dir/photos" ] && { [ "$PHOTOS" = 1 ] || [ "$(date +%u)" = 7 ]; }; then
  encrypt "ocs-photos-$DATE.tar.age" -C "$data_dir/photos" --exclude='*.tmp' -cf - .
fi

find "$backup_dir" -maxdepth 1 -type f -name 'ocs-*.age' -mtime +30 -delete || STATUS=1

# ---- mail ----
envelope() {   # "Name <a@b>" -> a@b
  local s=$1
  if [[ "$s" == *'<'*'>'* ]]; then s=${s#*<}; s=${s%%>*}; fi
  printf '%s' "$s"
}
FROM_ENV=$(envelope "$mail_from")
[ -n "$FROM_ENV" ] || FROM_ENV=$backup_mail_to
BOUNDARY="ocs-backup-$(date +%s)-$$"
MSG=$(mktemp "${TMPDIR:-/tmp}/ocs-backup-mail.XXXXXX")
trap 'rm -f "$MSG"' EXIT
{
  printf 'From: %s\n' "${mail_from:-$FROM_ENV}"
  printf 'To: %s\n' "$backup_mail_to"
  printf 'Subject: open-carshare Backup %s\n' "$DATE"
  printf 'Date: %s\n' "$(LC_ALL=C date -R)"
  printf 'MIME-Version: 1.0\n'
  printf 'Content-Type: multipart/mixed; boundary="%s"\n\n' "$BOUNDARY"
  printf -- '--%s\n' "$BOUNDARY"
  printf 'Content-Type: text/plain; charset=UTF-8\nContent-Transfer-Encoding: 8bit\n\n'
  printf 'Backup vom %s, verschlüsselt mit age.\n\n' "$DATE"
  for f in "${NEW[@]}"; do
    size=$(stat -c %s "$backup_dir/$f")
    if [ "$size" -le "$backup_max_mail_bytes" ]; then
      printf '%s: %s Bytes, angehängt.\n' "$f" "$size"
    else
      printf '%s: %s Bytes, zu groß für die Mail, nur lokal unter %s gespeichert.\n' "$f" "$size" "$backup_dir"
    fi
  done
  [ "$STATUS" = 0 ] || printf '\nACHTUNG: Mindestens ein Schritt ist fehlgeschlagen, siehe journalctl -u ocs-backup.\n'
  for f in "${NEW[@]}"; do
    size=$(stat -c %s "$backup_dir/$f")
    [ "$size" -le "$backup_max_mail_bytes" ] || continue
    printf -- '\n--%s\n' "$BOUNDARY"
    printf 'Content-Type: application/octet-stream; name="%s"\n' "$f"
    printf 'Content-Disposition: attachment; filename="%s"\n' "$f"
    printf 'Content-Transfer-Encoding: base64\n\n'
    base64 -w 76 "$backup_dir/$f"
  done
  printf -- '\n--%s--\n' "$BOUNDARY"
} > "$MSG"

if [ -n "${OCS_BACKUP_MAIL_FILE:-}" ]; then
  cp "$MSG" "$OCS_BACKUP_MAIL_FILE"
else
  [ -n "$smtp_url" ] || { echo "ocs-backup: smtp_url missing in $CONF" >&2; exit 1; }
  host=${smtp_url#*://}
  host=${host#*@}
  host=${host%%[:/]*}
  ssl=(--ssl-reqd)
  case "$host" in localhost|127.0.0.1) ssl=() ;; esac
  esc() { local s=${1//\\/\\\\}; printf '%s' "${s//\"/\\\"}"; }
  # Credentials go through a curl config on a file descriptor, never into argv.
  if ! curl --silent --show-error --crlf "${ssl[@]}" --url "$smtp_url" --mail-from "$FROM_ENV" \
       --mail-rcpt "$backup_mail_to" --upload-file "$MSG" \
       -K <( [ -n "$smtp_user" ] && printf 'user = "%s:%s"\n' "$(esc "$smtp_user")" "$(esc "$smtp_pass")" ); then
    echo "ocs-backup: sending the mail failed" >&2
    STATUS=1
  fi
fi
exit "$STATUS"
