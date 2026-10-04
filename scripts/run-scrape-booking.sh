#!/usr/bin/env bash
#
# run-scrape-booking.sh — start a local OpenCode server and run the
# opencode-marionette controller on an authenticated Booking.com session.
#
# Flow:
#   1. `opencode serve` on 127.0.0.1:PORT (background, private work dir).
#   2. Capture the server password from its log, probe authenticated
#      readiness, and export OPENCODE_SERVER_USERNAME / OPENCODE_SERVER_PASSWORD
#      / OPENCODE_BASE_URL for the controller (never printed).
#   3. Prepare Booking login in the controller's own session, then let OpenCode
#      choose permitted buttons/links from a trusted, bounded decisions policy.
#      scrape-endpoints discovers APIs before/after actions; schema-grabber
#      enriches request/response schemas and typed URL parameters for export.
#      Live runs force the assistant-browser handoff and use the Joe Litty
#      Rooms XPath success beacon; exports keep booking.com endpoints only.
#   4. Verify `x-prowsetk-marionette: used: true` and both output artifacts.
#   5. Stop the server unless --keep-server.
#
# Offline page (no booking credentials or page network; OpenCode still required):
#   scripts/run-scrape-booking.sh --html '<script>fetch("/api/orders")</script>'
#
# Live runs need booking credentials exactly like the driver itself
# (dotenv, environment, or --cookies-json); see
# examples/booking-dotcom-admin-scrape/Prowse.toml.
#
# Usage:
#   scripts/run-scrape-booking.sh [options]
#
# Options:
#   --port N               opencode port (default: 4096)
#   --host H               opencode bind host (default: 127.0.0.1)
#   --server-bin PATH      opencode binary (default: opencode from PATH)
#   --work-dir DIR         server work dir (default: fresh mktemp -d)
#   --keep-server          leave the server running after the scrape
#   --no-server            reuse an existing server (needs OPENCODE_BASE_URL
#                          or --host/--port plus OPENCODE_SERVER_PASSWORD)
#   --html STR             offline snippet instead of a live crawl
#   --url URL              start URL (default: https://admin.booking.com/)
#   --output PATH          OpenAPI YAML (default: _scraped/booking-dotcom-admin/
#                          BookingDotcomAdminPanel.yaml under the repo)
#   --postman PATH         Postman JSON (default: beside the YAML)
#   --decisions PATH       trusted action policy (default: Booking example's
#                          marionette-decisions.json; customize site selectors)
#   --marionette-bin PATH  ptk-opencode-marionette executable
#   --max-steps N          action budget (policy default: 24, hard cap: 64)
#   --max-page-requests N  page request budget (policy default: 512)
#   --max-get-probes N     GET schema probe budget (policy default: 64;
#                          offline pages always use 0)
#   --dotenv PATH          dotenv file with booking credentials
#   --cookies-json PATH    imported/Firefox-exported session cookies (default:
#                          _scraped/booking-dotcom-admin/cookies.json)
#   --assistant_browser_force BOOL  force the assistant-browser handoff
#                          (default: true)
#   --success_beacon QUERY login confirmation beacon (default:
#                          xpath=//h1[contains(normalize-space(.), 'Joe Litty Rooms')])
#   --success_beacon_type TYPE  beacon syntax (default: xpath)
#   --no-xcors [BOOL]      keep only booking.com TLD endpoints (default: true)
#   --opencode-max-requests N  bridge request budget, polls included
#                          (default: 512)
#   --opencode-wait-ms MS  cap on waiting for an agent reply
#                          (default: 300000)
#   -v, --verbose          stage, request, proxy, cookie and login diagnostics
#                          (--verbse is accepted as an alias)
#   -h, --help             show this help and exit
#
# Environment:
#   OPENCODE_SERVER_USERNAME  bridge auth user (default: opencode)
#   OPENCODE_SERVER_PASSWORD  with --no-server: bridge auth password,
#                             otherwise captured from the private server log
#   PROWSETK_MARIONETTE_BIN    controller executable (default:
#                             build/default/plugins/opencode-marionette/
#                             ptk-opencode-marionette under the repo)
#   HTTPS_PROXY / HTTP_PROXY  proxies for HTTPS / HTTP page requests
#   NO_PROXY / no_proxy       comma-separated proxy exclusions; loopback is
#                             always added for the local OpenCode server
#   FIREFOX_PROFILE_DIR       exact Firefox profile for cookie capture
#
# Only caller-permitted actions run; model output is never executable code.
# Schemas retain provenance and redaction. Coverage is explicitly incomplete.

set -euo pipefail
umask 077

HOST="127.0.0.1"
PORT="4096"
SERVER_BIN="opencode"
WORK_DIR=""
KEEP_SERVER=0
NO_SERVER=0
VERBOSE=0
HTML=""
URL="https://admin.booking.com/"
OUTPUT=""
POSTMAN=""
DOTENV=""
COOKIES_JSON=""
ASSISTANT_BROWSER_FORCE="true"
SUCCESS_BEACON="xpath=//h1[contains(normalize-space(.), 'Joe Litty Rooms')]"
SUCCESS_BEACON_TYPE="xpath"
NO_XCORS="true"
OPENCODE_MAX_REQUESTS="512"
OPENCODE_WAIT_MS="300000"
DECISIONS=""
MAX_STEPS=""
MAX_PAGE_REQUESTS=""
MAX_GET_PROBES=""
MARIONETTE_BIN="${PROWSETK_MARIONETTE_BIN:-}"

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

log() {
    echo "run-scrape-booking: $*" >&2
}

debug_log() {
    if [[ "$VERBOSE" -eq 1 ]]; then log "$*"; fi
}

die() {
    echo "run-scrape-booking: error: $*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port) PORT="${2:?--port needs a value}"; shift 2 ;;
        --host) HOST="${2:?--host needs a value}"; shift 2 ;;
        --server-bin) SERVER_BIN="${2:?--server-bin needs a value}"; shift 2 ;;
        --work-dir) WORK_DIR="${2:?--work-dir needs a value}"; shift 2 ;;
        --keep-server) KEEP_SERVER=1; shift ;;
        --no-server) NO_SERVER=1; shift ;;
        --html) HTML="${2:?--html needs a value}"; shift 2 ;;
        --url) URL="${2:?--url needs a value}"; shift 2 ;;
        --output) OUTPUT="${2:?--output needs a value}"; shift 2 ;;
        --postman) POSTMAN="${2:?--postman needs a value}"; shift 2 ;;
        --decisions) DECISIONS="${2:?--decisions needs a value}"; shift 2 ;;
        --marionette-bin) MARIONETTE_BIN="${2:?--marionette-bin needs a value}"; shift 2 ;;
        --max-steps) MAX_STEPS="${2:?--max-steps needs a value}"; shift 2 ;;
        --max-page-requests) MAX_PAGE_REQUESTS="${2:?--max-page-requests needs a value}"; shift 2 ;;
        --max-get-probes|--max-schema-probes) MAX_GET_PROBES="${2:?--max-get-probes needs a value}"; shift 2 ;;
        --dotenv) DOTENV="${2:?--dotenv needs a value}"; shift 2 ;;
        --cookies-json) COOKIES_JSON="${2:?--cookies-json needs a value}"; shift 2 ;;
        --assistant_browser_force|--assistant-browser-force)
            ASSISTANT_BROWSER_FORCE="${2:?--assistant_browser_force needs a value}"; shift 2 ;;
        --success_beacon|--success-beacon)
            SUCCESS_BEACON="${2:?--success_beacon needs a value}"; shift 2 ;;
        --success_beacon_type|--success-beacon-type)
            SUCCESS_BEACON_TYPE="${2:?--success_beacon_type needs a value}"; shift 2 ;;
        --no-xcors|--no_xcors)
            NO_XCORS="true"
            if [[ "${2:-}" == "true" || "${2:-}" == "false" ]]; then
                NO_XCORS="$2"; shift
            fi
            shift ;;
        --opencode-max-requests) OPENCODE_MAX_REQUESTS="${2:?needs a value}"; shift 2 ;;
        --opencode-wait-ms) OPENCODE_WAIT_MS="${2:?needs a value}"; shift 2 ;;
        -v|--verbose|--verbse) VERBOSE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "$MARIONETTE_BIN" ]]; then
    MARIONETTE_BIN="$REPO_ROOT/build/default/plugins/opencode-marionette/ptk-opencode-marionette"
fi
[[ -x "$MARIONETTE_BIN" ]] || die "opencode-marionette runner not found at $MARIONETTE_BIN
  build it first: cmake --preset default && cmake --build --preset default"
[[ -n "$DECISIONS" ]] || DECISIONS="$REPO_ROOT/examples/booking-dotcom-admin-scrape/marionette-decisions.json"
[[ -r "$DECISIONS" ]] || die "decisions policy is unreadable: $DECISIONS"
[[ -n "$OUTPUT" ]] || OUTPUT="$REPO_ROOT/_scraped/booking-dotcom-admin/BookingDotcomAdminPanel.yaml"
if [[ -z "$POSTMAN" ]]; then
    OUTPUT_BASE="$OUTPUT"
    if [[ "$OUTPUT" == *.yaml || "$OUTPUT" == *.yml ]]; then OUTPUT_BASE="${OUTPUT%.*}"; fi
    POSTMAN="${OUTPUT_BASE}.postman_collection.json"
fi
[[ -n "$COOKIES_JSON" ]] || COOKIES_JSON="$REPO_ROOT/_scraped/booking-dotcom-admin/cookies.json"
for value in "$ASSISTANT_BROWSER_FORCE" "$NO_XCORS"; do
    [[ "$value" == "true" || "$value" == "false" ]] || die "boolean options require true or false"
done
command -v curl >/dev/null 2>&1 || die "curl is required for server readiness probes"

# Keep local agent authentication on loopback even when page traffic uses a
# proxy. Preserve caller exclusions and provide both spellings for curl/core.
export NO_PROXY="${NO_PROXY:-${no_proxy:-}}"
if [[ -n "$NO_PROXY" && "$NO_PROXY" != *, ]]; then NO_PROXY+=","; fi
export NO_PROXY="${NO_PROXY}localhost,127.0.0.1,::1"
export no_proxy="$NO_PROXY"
debug_log "verbose diagnostics enabled (page/cookie/credential values are omitted)"
debug_log "proxy environment: HTTPS_PROXY=$([[ -n "${HTTPS_PROXY:-${https_proxy:-}}" ]] && echo set || echo unset), HTTP_PROXY=$([[ -n "${HTTP_PROXY:-${http_proxy:-}}" ]] && echo set || echo unset); local OpenCode bypasses proxies"

BASE_URL="http://${HOST}:${PORT}"
SERVER_PID=""
SERVER_LOG=""
CREATED_WORK_DIR=""

cleanup() {
    local status=$?
    if [[ "$KEEP_SERVER" -eq 0 && -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        log "stopping opencode server (pid $SERVER_PID)"
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "$CREATED_WORK_DIR" && "$KEEP_SERVER" -eq 0 ]]; then
        if [[ "$status" -eq 0 && "$VERBOSE" -eq 0 ]]; then
            rm -rf "$CREATED_WORK_DIR"
        else
            log "private server log retained at $SERVER_LOG"
        fi
    fi
    return "$status"
}
trap cleanup EXIT

if [[ "$NO_SERVER" -eq 1 ]]; then
    if [[ -n "${OPENCODE_BASE_URL:-}" ]]; then
        BASE_URL="$OPENCODE_BASE_URL"
    fi
    log "reusing existing OpenCode server"
else
    command -v "$SERVER_BIN" >/dev/null 2>&1 || die "opencode binary not found: $SERVER_BIN"
    if [[ -z "$WORK_DIR" ]]; then
        WORK_DIR="$(mktemp -d -t prowsetk-opencode-XXXXXX)"
        CREATED_WORK_DIR="$WORK_DIR"
    else
        mkdir -p "$WORK_DIR"
    fi
    SERVER_LOG="$WORK_DIR/opencode-server.log"
    log "starting $SERVER_BIN serve on ${HOST}:${PORT} (log: $SERVER_LOG)"
    # `serve` takes no work-dir argument; run it with cwd set instead.
    # Resolve the binary first so a relative --server-bin survives the cd.
    SERVER_BIN_ABS="$(realpath "$(command -v "$SERVER_BIN")")" || die "opencode binary not found: $SERVER_BIN"
    (cd "$WORK_DIR" && exec "$SERVER_BIN_ABS" serve --hostname "$HOST" --port "$PORT") \
        >"$SERVER_LOG" 2>&1 &
    SERVER_PID="$!"

    # The server prints "server password <secret>" on startup; poll for it.
    SERVER_PASSWORD=""
    for _ in $(seq 1 60); do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            die "opencode server exited during startup (private log: $SERVER_LOG)"
        fi
        SERVER_PASSWORD="$(sed -n 's/.*server password //p' "$SERVER_LOG" | tail -1)"
        if [[ -n "$SERVER_PASSWORD" ]]; then break; fi
        sleep 1
    done
    [[ -n "$SERVER_PASSWORD" ]] || die "no server password appeared in $SERVER_LOG"

    export OPENCODE_SERVER_USERNAME="${OPENCODE_SERVER_USERNAME:-opencode}"
    export OPENCODE_SERVER_PASSWORD="$SERVER_PASSWORD"
    export OPENCODE_BASE_URL="$BASE_URL"

    # Authenticated readiness: the v2 session endpoint answers 200.
    for _ in $(seq 1 60); do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            die "opencode server exited during startup (private log: $SERVER_LOG)"
        fi
        if curl -s -m 5 -o /dev/null -w "%{http_code}" \
                -u "${OPENCODE_SERVER_USERNAME}:${OPENCODE_SERVER_PASSWORD}" \
                "${BASE_URL}/api/session/active" | grep -q "^200$"; then
            break
        fi
        sleep 1
    done
    curl -s -m 5 -o /dev/null -w "%{http_code}" \
        -u "${OPENCODE_SERVER_USERNAME}:${OPENCODE_SERVER_PASSWORD}" \
        "${BASE_URL}/api/session/active" | grep -q "^200$" \
        || die "opencode server at $BASE_URL is not answering authenticated /api/session/active"
    log "opencode server ready at $BASE_URL"
fi

export OPENCODE_SERVER_USERNAME="${OPENCODE_SERVER_USERNAME:-opencode}"
export OPENCODE_BASE_URL="$BASE_URL"

RUNNER_ARGS=("$DECISIONS" "$URL" "$OUTPUT" "$POSTMAN"
    --booking-config "$REPO_ROOT/examples/booking-dotcom-admin-scrape/Prowse.toml"
    --opencode-api-prefix /api
    --assistant-browser-force "$ASSISTANT_BROWSER_FORCE"
    --success-beacon "$SUCCESS_BEACON"
    --success-beacon-type "$SUCCESS_BEACON_TYPE"
    --no-xcors "$NO_XCORS"
    --opencode-max-requests "$OPENCODE_MAX_REQUESTS"
    --opencode-wait-ms "$OPENCODE_WAIT_MS"
    --cookies-json "$COOKIES_JSON")
[[ -n "$HTML" ]] && RUNNER_ARGS+=(--html "$HTML")
[[ -n "$DOTENV" ]] && RUNNER_ARGS+=(--dotenv "$DOTENV")
[[ -n "$MAX_STEPS" ]] && RUNNER_ARGS+=(--max-steps "$MAX_STEPS")
[[ -n "$MAX_PAGE_REQUESTS" ]] && RUNNER_ARGS+=(--max-page-requests "$MAX_PAGE_REQUESTS")
[[ -n "$MAX_GET_PROBES" ]] && RUNNER_ARGS+=(--max-get-probes "$MAX_GET_PROBES")
[[ "$VERBOSE" -eq 1 ]] && RUNNER_ARGS+=(--verbose)

log "running opencode-marionette with scrape-endpoints discovery and schema-grabber enrichment"
if "$MARIONETTE_BIN" "${RUNNER_ARGS[@]}"; then
    debug_log "controller completed successfully; checking both exports"
else
    status=$?
    log "controller failed (exit $status); see the stage and login diagnostics above"
    exit "$status"
fi

[[ -s "$OUTPUT" ]] || die "expected output YAML not found: $OUTPUT"
[[ -s "$POSTMAN" ]] || die "expected Postman JSON not found: $POSTMAN"

if awk '/^x-prowsetk-marionette:/{inblock=1; next} inblock&&/^  used: true$/{found=1} inblock&&/^[^ ]/{inblock=0} END{exit !found}' "$OUTPUT"; then
    log "marionette exploration and schema export finished: $OUTPUT and $POSTMAN"
else
    die "runner finished without marionette metadata in $OUTPUT"
fi

if [[ "$KEEP_SERVER" -eq 1 && -n "$SERVER_PID" ]]; then
    log "keeping opencode server (pid $SERVER_PID, log $SERVER_LOG)"
fi
