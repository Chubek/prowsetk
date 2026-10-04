#!/usr/bin/env bash
set -euo pipefail
umask 077

output=${1:?usage: grab-firefox-cookies.sh OUTPUT_JSON [--verbose]}
shift
verbose=0
for option in "$@"; do
    case "$option" in
        -v|--verbose|--verbse) verbose=1 ;;
        *) echo "grab-firefox-cookies: unknown option" >&2; exit 2 ;;
    esac
done
firefox_root=${FIREFOX_PROFILE_ROOT:-"${HOME}/.mozilla/firefox"}

die() {
    echo "grab-firefox-cookies: $*; existing export preserved" >&2
    exit 1
}
log() {
    if [[ "$verbose" -eq 1 ]]; then echo "grab-firefox-cookies: $*" >&2; fi
}
command -v sqlite3 >/dev/null 2>&1 || die "sqlite3 is required"

profile_db=""
if [[ -n "${FIREFOX_PROFILE_DIR:-}" ]]; then
    profile_db="$FIREFOX_PROFILE_DIR/cookies.sqlite"
    [[ -r "$profile_db" ]] || die "FIREFOX_PROFILE_DIR has no readable cookies.sqlite"
else
    [[ -d "$firefox_root" ]] || die "Firefox profile root is unavailable; set FIREFOX_PROFILE_ROOT or FIREFOX_PROFILE_DIR"
    # An open Firefox normally writes new cookies to its WAL, not the main DB.
    # Rank both files so an older, inactive profile cannot win merely because
    # its main database was checkpointed more recently.
    newest=""
    while IFS= read -r -d '' candidate; do
        modified=$(stat -c %y "$candidate") || continue
        if [[ -f "$candidate-wal" ]]; then
            wal_modified=$(stat -c %y "$candidate-wal" 2>/dev/null) || wal_modified="$modified"
            if [[ "$wal_modified" > "$modified" ]]; then modified=$wal_modified; fi
        fi
        if [[ "$modified" > "$newest" ]]; then
            newest=$modified
            profile_db="$candidate"
        fi
    done < <(find "$firefox_root" -type f -name cookies.sqlite -print0 2>/dev/null)
    [[ -n "$profile_db" ]] || die "no Firefox cookies.sqlite found; set FIREFOX_PROFILE_DIR to the active browser profile"
fi
profile_db=$(realpath "$profile_db")
log "snapshotting $profile_db (override selection with FIREFOX_PROFILE_DIR)"

parent=$(dirname "$output")
mkdir -p "$parent" || die "cannot create export directory"
tmp_dir=$(mktemp -d "$parent/.firefox-cookies-XXXXXX")
trap 'rm -rf "$tmp_dir"' EXIT
# SQLite's online backup includes uncheckpointed WAL data while Firefox stays
# open. A relative destination avoids interpreting host paths as SQLite code.
if ! (cd "$tmp_dir" && sqlite3 "$profile_db" '.backup cookies.sqlite' >/dev/null 2>&1); then
    die "Firefox cookie snapshot failed"
fi
tmp_db="$tmp_dir/cookies.sqlite"
predicate="(lower(host) IN ('booking.com', '.booking.com') OR lower(host) LIKE '%.booking.com') AND (expiry = 0 OR expiry > CAST(strftime('%s', 'now') AS INTEGER))"
count=$(sqlite3 "$tmp_db" "SELECT count(*) FROM moz_cookies WHERE $predicate;" 2>/dev/null) || die "Firefox cookie database is unreadable"
[[ "$count" =~ ^[0-9]+$ && "$count" -gt 0 ]] || die "no unexpired Booking.com cookies found in the selected profile"

# The importer requires JSON booleans, not SQLite's integer 0/1 flags.
sqlite3 "$tmp_db" "SELECT json_group_array(json_object(
    'domain', host, 'path', path, 'name', name, 'value', value, 'expires', expiry,
    'secure', json(CASE WHEN isSecure THEN 'true' ELSE 'false' END),
    'httpOnly', json(CASE WHEN isHttpOnly THEN 'true' ELSE 'false' END)))
    FROM moz_cookies WHERE $predicate;" > "$tmp_dir/cookies.json" 2>/dev/null || die "Firefox cookie export failed"
mv -f "$tmp_dir/cookies.json" "$output" || die "cannot install cookie export"
log "exported $count unexpired Booking.com cookies; values omitted"
