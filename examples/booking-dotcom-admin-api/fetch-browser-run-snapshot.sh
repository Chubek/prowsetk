#!/bin/sh
# Fetch a rendered admin.booking.com snapshot via Cloudflare Browser Run
# (plugins/browser-run-integration) for the booking-admin-api pipeline.
#
# This is an alternative snapshot supplier next to the default Qutebrowser
# assistant browser: remote Chromium renders the page (full JS engine for pages
# Flatworm's partial shim cannot cover), and the returned HTML feeds the same
# downstream consumers unchanged:
#   prowsetk run booking-admin-api --html "$(cat SNAP.html)" ...
#   run-opencode-marionette.sh --snapshot SNAP.html ...
# The snapshot Session stays JavaScript-disabled; no cookies, JS state, HTTP
# status or response bodies transfer with the import. Positive DOM login
# evidence is still required for live exports. Nothing here bypasses CAPTCHAs:
# satisfy any challenge in the remote session first, then re-snapshot.
#
# Credentials (CLOUDFLARE_ACCOUNT_ID plus an API token with Browser Rendering
# - Edit, or the Cloudflare-bound oauth-assist cache) are control-plane only:
# sent as a Bearer header to the fixed Cloudflare API origin, never in page
# URLs or Session headers. Environment values override Prowse.toml [cloudflare].
#
# Usage:
#   fetch-browser-run-snapshot.sh [--output SNAP.html]
#     [--url https://admin.booking.com/] [--config Prowse.toml]
#     [--browser-run-bin BIN]
#
# --output creates a new owner-only (0600) file and refuses existing ones; the
# snapshot may contain private page values, so store it accordingly.
set -eu

EXAMPLE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CONFIG="$EXAMPLE_DIR/Prowse.toml"
URL="https://admin.booking.com/"
OUTPUT="_scraped/booking-admin-browser-run.html"
BIN="${PROWSETK_BROWSER_RUN_BIN:-}"

# Print the header's usage block by marker, so editing the comments above
# cannot silently truncate or overrun the help text.
usage() { awk '/^# Usage:/{f=1} f && /^#/{sub(/^# ?/,""); print; next} f{exit}' "$0"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --output) OUTPUT="${2:?missing value for $1}"; shift 2 ;;
        --url) URL="${2:?missing value for $1}"; shift 2 ;;
        --config) CONFIG="${2:?missing value for $1}"; shift 2 ;;
        --browser-run-bin) BIN="${2:?missing value for $1}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "fetch-browser-run-snapshot.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

case "$URL" in
    https://admin.booking.com*) ;;
    *) echo "fetch-browser-run-snapshot.sh: URL must stay on https://admin.booking.com" >&2; exit 2 ;;
esac
if [ ! -f "$CONFIG" ]; then
    echo "fetch-browser-run-snapshot.sh: config file unreadable" >&2
    exit 1
fi
if [ -e "$OUTPUT" ]; then
    echo "fetch-browser-run-snapshot.sh: output already exists (remove it first): $OUTPUT" >&2
    exit 1
fi

if [ -z "$BIN" ]; then
    for candidate in \
        "$EXAMPLE_DIR/../../build/default/plugins/browser-run-integration/ptk-browser-run" \
        "$EXAMPLE_DIR/../../build/release/plugins/browser-run-integration/ptk-browser-run"; do
        if [ -x "$candidate" ]; then BIN="$candidate"; break; fi
    done
fi
if [ -z "$BIN" ]; then
    BIN=$(command -v ptk-browser-run || true)
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "fetch-browser-run-snapshot.sh: ptk-browser-run not found (build with the default preset or set PROWSETK_BROWSER_RUN_BIN)" >&2
    exit 1
fi

exec "$BIN" content --config "$CONFIG" --url "$URL" --output "$OUTPUT"
