#!/usr/bin/env bash
set -euo pipefail

output=${1:?usage: grab-firefox-cookies.sh OUTPUT_JSON}
firefox_root=${FIREFOX_PROFILE_ROOT:-"${HOME}/.mozilla/firefox"}

profile_db="$(find "$firefox_root" -type f -name cookies.sqlite -printf '%T@ %p\n' 2>/dev/null |
  sort -nr | sed -n '1s/^[^ ]* //p')"
if [[ -z "$profile_db" ]]; then
  echo "no Firefox cookies.sqlite found under $firefox_root" >&2
  exit 1
fi

mkdir -p "$(dirname "$output")"
tmp_db="$(mktemp)"
trap 'rm -f "$tmp_db"' EXIT
sqlite3 "$profile_db" ".backup '$tmp_db'"

sqlite3 -json "$tmp_db" \
  "SELECT host AS domain, path, name, value, expiry AS expires, isSecure AS secure, isHttpOnly AS httpOnly FROM moz_cookies WHERE host LIKE '%booking.com' OR host LIKE '%.booking.com';" \
  > "$output"
