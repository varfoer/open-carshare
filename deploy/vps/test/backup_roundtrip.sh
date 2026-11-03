#!/usr/bin/env bash
# Round trip of deploy/vps/ocs-backup.sh and ocs-restore.sh (ctest backup_roundtrip).
# usage: backup_roundtrip.sh <path to ocs-server>
# Exit 77 (skipped) when age/age-keygen are missing.
set -euo pipefail

SERVER=${1:?usage: backup_roundtrip.sh <ocs-server>}
HERE=$(cd "$(dirname "$0")" && pwd)
BACKUP="$HERE/../ocs-backup.sh"
RESTORE="$HERE/../ocs-restore.sh"
command -v age >/dev/null && command -v age-keygen >/dev/null || { echo "age not installed: skipped"; exit 77; }

T=$(mktemp -d "${TMPDIR:-/tmp}/ocs-backup-test.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

MARK=Plainmarker7f3aQ
D="$T/data"
mkdir -p "$D/photos"
cat > "$D/users.jsonl" <<EOF
{"id":1,"email":"$MARK@x.de","name":"$MARK","street":"S","zip":"1","city":"C","pw_hash":"","role":"admin","active":true,"license_checked_at":0,"created_at":1}
EOF
cat > "$D/bookings.jsonl" <<'EOF'
{"id":1,"car_id":1,"user_id":1,"booked_start":1774742400,"booked_end":1774749600,"state":"reserved","cancelled_at":0,"cancel_fee_cents":0,"created_at":1,"modified_at":1,"modified_by":1}
EOF
echo "half-written $MARK" > "$D/x.tmp"
head -c 3000 /dev/urandom > "$D/photos/r1-1.jpg"

age-keygen -o "$T/key.txt" 2>/dev/null
RCPT=$(age-keygen -y "$T/key.txt")

# conf <file> <backup_dir> [extra lines]
conf() {
  cat > "$1" <<EOF
# test conf
data_dir = $D
web_dir = $T/web
base_url = https://carshare.test
smtp_url = smtp://127.0.0.1:2525
mail_from = open-carshare <ocs@carshare.test>
backup_recipient = $RCPT
backup_mail_to = admin@carshare.test
backup_dir = $2
${3:-}
EOF
}

TODAY=$(date +%F)
DATA_AGE="ocs-data-$TODAY.tar.gz.age"
PHOTO_AGE="ocs-photos-$TODAY.tar.age"

# extract <mail file> <attachment filename> <out>: base64 body of that part, decoded.
extract() {
  awk -v fn="filename=\"$2\"" '
    /^--/ { if (inb) exit; next }
    index($0, fn) { want = 1; next }
    want && /^\r?$/ { inb = 1; want = 0; next }
    inb { print }
  ' "$1" | base64 -d > "$3"
}

# (a) daily run
conf "$T/a.conf" "$T/bk-a"
mkdir -p "$T/bk-a"
OCS_BACKUP_MAIL_FILE="$T/mail-a.eml" bash "$BACKUP" -c "$T/a.conf" || fail "(a) backup exited $?"
n=$(find "$T/bk-a" -name 'ocs-data-*.tar.gz.age' | wc -l)
[ "$n" -eq 1 ] || fail "(a) expected one data archive, found $n"
[ -f "$T/bk-a/$DATA_AGE" ] || fail "(a) $DATA_AGE missing"
[ "$(stat -c %a "$T/bk-a/$DATA_AGE")" = 600 ] || fail "(a) mode is not 600"
if [ "$(date +%u)" != 7 ]; then
  [ -z "$(find "$T/bk-a" -name 'ocs-photos-*')" ] || fail "(a) photos archive on a weekday"
fi
grep -q "Content-Disposition: attachment; filename=\"$DATA_AGE\"" "$T/mail-a.eml" || fail "(a) no attachment header"
grep -q "^Subject: open-carshare Backup $TODAY" "$T/mail-a.eml" || fail "(a) subject"
grep -q "^To: admin@carshare.test" "$T/mail-a.eml" || fail "(a) recipient"

# (b) restore from the mailed attachment, then check-data
mkdir -p "$T/from-mail"
extract "$T/mail-a.eml" "$DATA_AGE" "$T/from-mail/$DATA_AGE"
cmp -s "$T/from-mail/$DATA_AGE" "$T/bk-a/$DATA_AGE" || fail "(b) mailed attachment differs from the local archive"
bash "$RESTORE" "$T/from-mail/$DATA_AGE" "$T/key.txt" "$T/restored" || fail "(b) restore exited $?"
mkdir -p "$T/expect"
cp "$D/users.jsonl" "$D/bookings.jsonl" "$T/expect/"
diff -r "$T/expect" "$T/restored" || fail "(b) restored data differs"
cat > "$T/restored.conf" <<EOF
data_dir = $T/restored
web_dir = $T/web
base_url = https://carshare.test
EOF
out=$("$SERVER" -c "$T/restored.conf" check-data) || fail "(b) check-data exited $?"
[ "$out" = "users 1, bookings 1, invoices 0, mandates 0" ] || fail "(b) check-data printed: $out"

# (c) --photos
conf "$T/c.conf" "$T/bk-c"
OCS_BACKUP_MAIL_FILE="$T/mail-c.eml" bash "$BACKUP" -c "$T/c.conf" --photos || fail "(c) backup exited $?"
[ -f "$T/bk-c/$PHOTO_AGE" ] || fail "(c) $PHOTO_AGE missing"
grep -q "filename=\"$PHOTO_AGE\"" "$T/mail-c.eml" || fail "(c) photos not attached"
bash "$RESTORE" "$T/bk-c/$PHOTO_AGE" "$T/key.txt" "$T/restored-photos" || fail "(c) restore exited $?"
diff -r "$D/photos" "$T/restored-photos" || fail "(c) restored photos differ"

# (d) attachment over the size cap
conf "$T/d.conf" "$T/bk-d" "backup_max_mail_bytes = 10"
OCS_BACKUP_MAIL_FILE="$T/mail-d.eml" bash "$BACKUP" -c "$T/d.conf" || fail "(d) backup exited $?"
grep -q "Content-Disposition: attachment" "$T/mail-d.eml" && fail "(d) attachment despite the cap"
grep -q "zu groß für die Mail" "$T/mail-d.eml" || fail "(d) no size note"
[ -f "$T/bk-d/$DATA_AGE" ] || fail "(d) local archive missing"

# (e) retention: older than 30 days deleted, younger kept
mkdir -p "$T/bk-e"
touch -d '40 days ago' "$T/bk-e/ocs-data-2000-01-01.tar.gz.age"
touch -d '10 days ago' "$T/bk-e/ocs-data-2000-01-02.tar.gz.age"
conf "$T/e.conf" "$T/bk-e"
OCS_BACKUP_MAIL_FILE="$T/mail-e.eml" bash "$BACKUP" -c "$T/e.conf" || fail "(e) backup exited $?"
[ -e "$T/bk-e/ocs-data-2000-01-01.tar.gz.age" ] && fail "(e) 40-day-old archive kept"
[ -e "$T/bk-e/ocs-data-2000-01-02.tar.gz.age" ] || fail "(e) 10-day-old archive deleted"
grep -q "ocs-data-2000-01-02" "$T/mail-e.eml" && fail "(e) old archive attached"

# (f) no plaintext anywhere the backup writes
for f in "$T"/bk-* "$T"/mail-*.eml; do
  c=$( { grep -rc "$MARK" "$f" || true; } | awk -F: '{s += $NF} END {print s + 0}')
  [ "$c" -eq 0 ] || fail "(f) plaintext marker found in $f"
done

# (g) restore refuses a non-empty target
mkdir -p "$T/busy" && echo x > "$T/busy/keep"
set +e
bash "$RESTORE" "$T/bk-a/$DATA_AGE" "$T/key.txt" "$T/busy" 2>/dev/null
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "(g) restore into a non-empty dir exited $rc, want 2"
[ "$(ls "$T/busy")" = keep ] || fail "(g) non-empty target was modified"

# missing backup_recipient -> exit 2
grep -v '^backup_recipient' "$T/a.conf" > "$T/norcpt.conf"
set +e
OCS_BACKUP_MAIL_FILE="$T/mail-x.eml" bash "$BACKUP" -c "$T/norcpt.conf" 2>/dev/null
rc=$?
set -e
[ "$rc" -eq 2 ] || fail "missing backup_recipient exited $rc, want 2"

echo "backup_roundtrip: ok"
