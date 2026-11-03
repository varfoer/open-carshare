#!/usr/bin/env bash
# Restores an ocs-backup.sh archive into an empty directory.
#
# usage: ocs-restore.sh <archive.age> <identity-file> <target-dir>
#
# <identity-file> is the admin's age private key (age-keygen output). The target must not
# exist or be empty; it is never merged into. Verify afterwards with
#   ocs-server -c <conf with data_dir = target> check-data
# Exit 0 ok, 2 usage error or non-empty target, 1 decryption/extraction failed.
set -euo pipefail
umask 077

[ $# -eq 3 ] || { echo "usage: ocs-restore.sh <archive.age> <identity-file> <target-dir>" >&2; exit 2; }
ARCHIVE=$1 IDENTITY=$2 TARGET=$3
[ -r "$ARCHIVE" ] || { echo "ocs-restore: cannot read $ARCHIVE" >&2; exit 2; }
[ -r "$IDENTITY" ] || { echo "ocs-restore: cannot read $IDENTITY" >&2; exit 2; }
if [ -e "$TARGET" ]; then
  [ -d "$TARGET" ] || { echo "ocs-restore: $TARGET is not a directory" >&2; exit 2; }
  [ -z "$(ls -A "$TARGET")" ] || { echo "ocs-restore: $TARGET is not empty" >&2; exit 2; }
fi
command -v age >/dev/null || { echo "ocs-restore: age not installed" >&2; exit 1; }

case "$ARCHIVE" in
  *.tar.gz.age) z=(-z) ;;
  *.tar.age) z=() ;;
  *) echo "ocs-restore: $ARCHIVE is neither .tar.gz.age nor .tar.age" >&2; exit 2 ;;
esac
mkdir -p "$TARGET"
if ! age -d -i "$IDENTITY" "$ARCHIVE" | tar -x "${z[@]}" -C "$TARGET"; then
  echo "ocs-restore: decryption or extraction failed; $TARGET may be partially filled" >&2
  exit 1
fi
echo "ocs-restore: restored into $TARGET"
