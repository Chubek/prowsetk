#!/bin/sh
# Run the OpenCode marionette over a Qutebrowser snapshot of admin.booking.com.
#
# The snapshot HTML is produced by the assistant browser (human login, MFA and
# any human-verification challenge are solved interactively in Qutebrowser;
# nothing here bypasses CAPTCHAs). OpenCode then chooses only among the
# allow-listed action IDs in marionette-decisions.json. Page text, cookies,
# headers and typing values are never sent to the agent.
#
# Usage:
#   run-opencode-marionette.sh --snapshot SNAP.html [--decisions FILE]
#     [--output OPENAPI.yaml] [--postman POSTMAN.json]
#     [--url https://admin.booking.com/] [--marionette-bin BIN]
#
# Requires a running OpenCode server (opencode serve). Fully offline page
# handling: GET schema probes are disabled for snapshot input and coverage is
# reported incomplete.
set -eu

EXAMPLE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DECISIONS="$EXAMPLE_DIR/marionette-decisions.json"
OUTPUT="_scraped/booking-admin-marionette-openapi.yaml"
POSTMAN="_scraped/booking-admin-marionette-postman.json"
URL="https://admin.booking.com/"
SNAPSHOT=""
BIN="${PROWSETK_MARIONETTE_BIN:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --snapshot) SNAPSHOT="${2:?missing value for $1}"; shift 2 ;;
        --decisions) DECISIONS="${2:?missing value for $1}"; shift 2 ;;
        --output) OUTPUT="${2:?missing value for $1}"; shift 2 ;;
        --postman) POSTMAN="${2:?missing value for $1}"; shift 2 ;;
        --url) URL="${2:?missing value for $1}"; shift 2 ;;
        --marionette-bin) BIN="${2:?missing value for $1}"; shift 2 ;;
        --help|-h)
            sed -n '1,20p' "$0"
            exit 0 ;;
        *) echo "run-opencode-marionette.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

if [ -z "$SNAPSHOT" ]; then
    echo "run-opencode-marionette.sh: --snapshot SNAP.html is required" >&2
    exit 2
fi
if [ ! -f "$SNAPSHOT" ]; then
    echo "run-opencode-marionette.sh: snapshot file unreadable" >&2
    exit 1
fi
if [ ! -f "$DECISIONS" ]; then
    echo "run-opencode-marionette.sh: decisions file unreadable" >&2
    exit 1
fi
case "$URL" in
    https://admin.booking.com*) ;;
    *) echo "run-opencode-marionette.sh: URL must stay on https://admin.booking.com" >&2; exit 2 ;;
esac

if [ -z "$BIN" ]; then
    for candidate in \
        "$EXAMPLE_DIR/../../build/default/plugins/opencode-marionette/ptk-opencode-marionette" \
        "$EXAMPLE_DIR/../../build/release/plugins/opencode-marionette/ptk-opencode-marionette"; do
        if [ -x "$candidate" ]; then BIN="$candidate"; break; fi
    done
fi
if [ -z "$BIN" ]; then
    BIN=$(command -v ptk-opencode-marionette || true)
fi
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "run-opencode-marionette.sh: ptk-opencode-marionette not found (build with the default preset or set PROWSETK_MARIONETTE_BIN)" >&2
    exit 1
fi

exec "$BIN" "$DECISIONS" "$URL" "$OUTPUT" "$POSTMAN" "$SNAPSHOT"
