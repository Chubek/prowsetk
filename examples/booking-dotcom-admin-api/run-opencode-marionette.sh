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
#     [--verbose] [--no-xcors BOOL] [--max-steps N] [--max-page-requests N]
#     [--opencode-max-requests N] [--opencode-wait-ms MS] [--opencode-api-prefix /api]
#     [--booking-config TOML] [--cookies-json FILE] [--dotenv FILE]
#     [--success-beacon QUERY] [--success-beacon-type TYPE]
#     [--assistant-browser-force BOOL] [--max-get-probes N]
#
# Snapshot options: --verbose, --no-xcors, --max-steps, --max-page-requests,
# --opencode-max-requests, --opencode-wait-ms and --opencode-api-prefix apply
# to any run. --no-xcors is only sent when given, so the runner's own default
# export policy is preserved otherwise. The remaining options configure the
# runner's Booking login preparation and are only accepted together with
# --booking-config, because they have no effect without it; that option needs a
# project whose driver is named booking-dotcom-admin.
#
# Snapshot input is offline with a deterministic transport, so the runner forces
# GET schema probes to zero: --max-get-probes only matters with --booking-config.
# Budget ranges are enforced by the runner, so an out-of-range value fails there;
# this wrapper rejects malformed values and unknown options with exit 2.
#
# Every value reaches the runner as one argument; nothing is re-split or
# expanded, so a selector or beacon containing spaces stays intact.
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

VERBOSE=0
NO_XCORS=""
MAX_STEPS=""
MAX_PAGE_REQUESTS=""
OPENCODE_MAX_REQUESTS=""
OPENCODE_WAIT_MS=""
OPENCODE_API_PREFIX=""
BOOKING_CONFIG=""
COOKIES_JSON=""
DOTENV=""
SUCCESS_BEACON=""
SUCCESS_BEACON_TYPE=""
ASSISTANT_BROWSER_FORCE=""
MAX_GET_PROBES=""

die() { echo "run-opencode-marionette.sh: $1" >&2; exit "${2:-1}"; }
# Print the header's usage block by marker, so editing the comments above
# cannot silently truncate or overrun the help text.
usage() { awk '/^# Usage:/{f=1} f && /^#/{sub(/^# ?/,""); print; next} f{exit}' "$0"; }

need_value() { [ "$2" -ge 2 ] || die "$1 needs a value" 2; }
# Reject anything the runner could not parse, without duplicating its caps.
digits() {
    case "$2" in
        ''|*[!0-9]*) die "$1 must be a non-negative integer" 2 ;;
    esac
}
boolean() {
    case "$2" in
        true|false) ;;
        *) die "$1 must be true or false" 2 ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --snapshot) need_value "$1" $#; SNAPSHOT="$2"; shift 2 ;;
        --decisions) need_value "$1" $#; DECISIONS="$2"; shift 2 ;;
        --output) need_value "$1" $#; OUTPUT="$2"; shift 2 ;;
        --postman) need_value "$1" $#; POSTMAN="$2"; shift 2 ;;
        --url) need_value "$1" $#; URL="$2"; shift 2 ;;
        --marionette-bin) need_value "$1" $#; BIN="$2"; shift 2 ;;
        -v|--verbose|--verbse) VERBOSE=1; shift ;;
        --no-xcors|--no_xcors)
            need_value "$1" $#; boolean "$1" "$2"; NO_XCORS="$2"; shift 2 ;;
        --max-steps) need_value "$1" $#; digits "$1" "$2"; MAX_STEPS="$2"; shift 2 ;;
        --max-page-requests)
            need_value "$1" $#; digits "$1" "$2"; MAX_PAGE_REQUESTS="$2"; shift 2 ;;
        --opencode-max-requests)
            need_value "$1" $#; digits "$1" "$2"; OPENCODE_MAX_REQUESTS="$2"; shift 2 ;;
        --opencode-wait-ms)
            need_value "$1" $#; digits "$1" "$2"; OPENCODE_WAIT_MS="$2"; shift 2 ;;
        --opencode-api-prefix)
            need_value "$1" $#
            [ "$2" = "/api" ] || die "$1 must be /api" 2
            OPENCODE_API_PREFIX="$2"; shift 2 ;;
        --booking-config) need_value "$1" $#; BOOKING_CONFIG="$2"; shift 2 ;;
        --cookies-json) need_value "$1" $#; COOKIES_JSON="$2"; shift 2 ;;
        --dotenv) need_value "$1" $#; DOTENV="$2"; shift 2 ;;
        --success-beacon|--success_beacon)
            need_value "$1" $#; SUCCESS_BEACON="$2"; shift 2 ;;
        --success-beacon-type|--success_beacon_type)
            need_value "$1" $#; SUCCESS_BEACON_TYPE="$2"; shift 2 ;;
        --assistant-browser-force|--assistant_browser_force)
            need_value "$1" $#; boolean "$1" "$2"; ASSISTANT_BROWSER_FORCE="$2"; shift 2 ;;
        --max-get-probes)
            need_value "$1" $#; digits "$1" "$2"; MAX_GET_PROBES="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) die "unknown option $1" 2 ;;
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

# Login-coupled options are meaningless without a login preparation config, so
# reject the combination rather than silently ignoring them.
if [ -z "$BOOKING_CONFIG" ]; then
    for coupled in "$COOKIES_JSON" "$DOTENV" "$SUCCESS_BEACON" "$SUCCESS_BEACON_TYPE" \
                    "$ASSISTANT_BROWSER_FORCE" "$MAX_GET_PROBES"; do
        [ -z "$coupled" ] || die "login options also require --booking-config" 2
    done
elif [ ! -f "$BOOKING_CONFIG" ]; then
    die "config file unreadable" 1
fi

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

# Assemble the argv with "set --" so each value stays exactly one argument: no
# word splitting, no pathname expansion, no shell evaluation.
set -- "$DECISIONS" "$URL" "$OUTPUT" "$POSTMAN" "$SNAPSHOT"
[ -z "$BOOKING_CONFIG" ] || set -- "$@" --booking-config "$BOOKING_CONFIG"
[ -z "$COOKIES_JSON" ] || set -- "$@" --cookies-json "$COOKIES_JSON"
[ -z "$DOTENV" ] || set -- "$@" --dotenv "$DOTENV"
[ -z "$SUCCESS_BEACON" ] || set -- "$@" --success-beacon "$SUCCESS_BEACON"
[ -z "$SUCCESS_BEACON_TYPE" ] || set -- "$@" --success-beacon-type "$SUCCESS_BEACON_TYPE"
[ -z "$ASSISTANT_BROWSER_FORCE" ] || set -- "$@" --assistant-browser-force "$ASSISTANT_BROWSER_FORCE"
[ -z "$MAX_GET_PROBES" ] || set -- "$@" --max-get-probes "$MAX_GET_PROBES"
[ -z "$NO_XCORS" ] || set -- "$@" --no-xcors "$NO_XCORS"
[ -z "$MAX_STEPS" ] || set -- "$@" --max-steps "$MAX_STEPS"
[ -z "$MAX_PAGE_REQUESTS" ] || set -- "$@" --max-page-requests "$MAX_PAGE_REQUESTS"
[ -z "$OPENCODE_MAX_REQUESTS" ] || set -- "$@" --opencode-max-requests "$OPENCODE_MAX_REQUESTS"
[ -z "$OPENCODE_WAIT_MS" ] || set -- "$@" --opencode-wait-ms "$OPENCODE_WAIT_MS"
[ -z "$OPENCODE_API_PREFIX" ] || set -- "$@" --opencode-api-prefix "$OPENCODE_API_PREFIX"
[ "$VERBOSE" -eq 0 ] || set -- "$@" --verbose

exec "$BIN" "$@"
