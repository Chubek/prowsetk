#!/usr/bin/env bash
#
# run-scrape-booking.sh — start a local OpenCode server and run the
# booking-dotcom-admin-scrape driver with agent endpoint cleanup enabled.
#
# Flow:
#   1. `opencode serve` on 127.0.0.1:PORT (background, private work dir).
#   2. Capture the server password from its log, probe authenticated
#      readiness, and export OPENCODE_SERVER_USERNAME / OPENCODE_SERVER_PASSWORD
#      / OPENCODE_BASE_URL for the driver (never printed).
#   3. `prowsetk run booking-dotcom-admin --opencode true ...` so scraped
#      endpoints go through the lopencode bridge cleanup before export.
#   4. Verify `x-prowsetk-opencode: used: true` in the YAML; fail loudly when
#      the agent pass did not run (stale predictions are worse than none).
#   5. Stop the server unless --keep-server.
#
# Offline (deterministic, no booking credentials needed):
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
#   --url URL              live start URL (default: driver default)
#   --output PATH          OpenAPI YAML output (default: driver default)
#   --postman PATH         Postman JSON output (default: driver default)
#   --dotenv PATH          dotenv file with booking credentials
#   --cookies-json PATH    imported session cookies
#   --opencode-max-requests N  bridge request budget, polls included
#                          (default: 90)
#   --opencode-wait-ms MS  cap on waiting for an agent reply
#                          (default: 300000)
#   --opencode-module PATH lopencode native module (default: in-repo dev
#                          build when present)
#   -h, --help             show this help and exit
#
# Environment:
#   OPENCODE_SERVER_USERNAME  bridge auth user (default: opencode)
#   OPENCODE_SERVER_PASSWORD  with --no-server: bridge auth password,
#                             otherwise captured from the server log
#   PROWSETK_BIN              prowsetk CLI (default:
#                             build/default/src/cli/prowsetk under the repo)

set -euo pipefail

HOST="127.0.0.1"
PORT="4096"
SERVER_BIN="opencode"
WORK_DIR=""
KEEP_SERVER=0
NO_SERVER=0
HTML=""
URL=""
OUTPUT=""
POSTMAN=""
DOTENV=""
COOKIES_JSON=""
OPENCODE_MAX_REQUESTS="90"
OPENCODE_WAIT_MS="300000"
OPENCODE_MODULE=""
PROWSETK_BIN="${PROWSETK_BIN:-}"

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

log() {
    echo "run-scrape-booking: $*" >&2
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
        --dotenv) DOTENV="${2:?--dotenv needs a value}"; shift 2 ;;
        --cookies-json) COOKIES_JSON="${2:?--cookies-json needs a value}"; shift 2 ;;
        --opencode-max-requests) OPENCODE_MAX_REQUESTS="${2:?needs a value}"; shift 2 ;;
        --opencode-wait-ms) OPENCODE_WAIT_MS="${2:?needs a value}"; shift 2 ;;
        --opencode-module) OPENCODE_MODULE="${2:?needs a value}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "$PROWSETK_BIN" ]]; then
    PROWSETK_BIN="$REPO_ROOT/build/default/src/cli/prowsetk"
fi
[[ -x "$PROWSETK_BIN" ]] || die "prowsetk CLI not found at $PROWSETK_BIN
  build it first: cmake --preset default && cmake --build --preset default"
command -v curl >/dev/null 2>&1 || die "curl is required for server readiness probes"

BASE_URL="http://${HOST}:${PORT}"
SERVER_PID=""
SERVER_LOG=""
CREATED_WORK_DIR=""

cleanup() {
    if [[ "$KEEP_SERVER" -eq 0 && -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        log "stopping opencode server (pid $SERVER_PID)"
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    if [[ -n "$CREATED_WORK_DIR" && "$KEEP_SERVER" -eq 0 ]]; then
        rm -rf "$CREATED_WORK_DIR"
    fi
}
trap cleanup EXIT

if [[ "$NO_SERVER" -eq 1 ]]; then
    if [[ -n "${OPENCODE_BASE_URL:-}" ]]; then
        BASE_URL="$OPENCODE_BASE_URL"
    fi
    log "reusing existing OpenCode server at $BASE_URL"
    if [[ -z "${OPENCODE_SERVER_PASSWORD:-}" ]]; then
        log "warning: OPENCODE_SERVER_PASSWORD is unset; the bridge will send no credentials (expect 401s and skipped cleanup)"
    fi
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
    SERVER_BIN_ABS="$(command -v "$SERVER_BIN")" || die "opencode binary not found: $SERVER_BIN"
    (cd "$WORK_DIR" && exec "$SERVER_BIN_ABS" serve --hostname "$HOST" --port "$PORT") \
        >"$SERVER_LOG" 2>&1 &
    SERVER_PID="$!"

    # The server prints "server password <secret>" on startup; poll for it.
    SERVER_PASSWORD=""
    for _ in $(seq 1 60); do
        if ! kill -0 "$SERVER_PID" 2>/dev/null; then
            die "opencode server exited during startup; log tail: $(tail -5 "$SERVER_LOG" | tr '\n' '|')"
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
            die "opencode server exited during startup; log tail: $(tail -5 "$SERVER_LOG" | tr '\n' '|')"
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

DRIVER_ARGS=(run booking-dotcom-admin
    --config "$REPO_ROOT/examples/booking-dotcom-admin-scrape/Prowse.toml"
    --opencode true
    --opencode_max_requests "$OPENCODE_MAX_REQUESTS"
    --opencode_wait_ms "$OPENCODE_WAIT_MS")
if [[ -z "$OPENCODE_MODULE" ]]; then
    OPENCODE_MODULE="$REPO_ROOT/build/default/plugins/opencode-bridge/lopencode.so"
fi
if [[ -f "$OPENCODE_MODULE" ]]; then
    DRIVER_ARGS+=(--opencode_module "$OPENCODE_MODULE")
fi
[[ -n "$HTML" ]] && DRIVER_ARGS+=(--html "$HTML")
[[ -n "$URL" ]] && DRIVER_ARGS+=(--url "$URL")
[[ -n "$OUTPUT" ]] && DRIVER_ARGS+=(--output "$OUTPUT")
[[ -n "$POSTMAN" ]] && DRIVER_ARGS+=(--postman "$POSTMAN")
[[ -n "$DOTENV" ]] && DRIVER_ARGS+=(--dotenv "$DOTENV")
[[ -n "$COOKIES_JSON" ]] && DRIVER_ARGS+=(--cookies-json "$COOKIES_JSON")

log "running booking-dotcom-admin driver with opencode cleanup"
"$PROWSETK_BIN" "${DRIVER_ARGS[@]}"

# Resolve the YAML we just wrote (explicit --output or the driver default).
YAML="$OUTPUT"
if [[ -z "$YAML" ]]; then
    YAML="$REPO_ROOT/_scraped/booking-dotcom-admin/BookingDotcomAdminPanel.yaml"
fi
[[ -f "$YAML" ]] || die "expected output YAML not found: $YAML"

if awk '/^x-prowsetk-opencode:/{inblock=1; next} inblock&&/^  used: true/{found=1} inblock&&/^[^ ]/{inblock=0} END{exit !found}' "$YAML"; then
    log "opencode cleanup ran (x-prowsetk-opencode used: true): $YAML"
    awk '/^x-prowsetk-opencode:/{inblock=1} inblock{print} inblock&&/^  note:/{exit}' "$YAML" >&2 || true
else
    die "driver finished but opencode cleanup did not run (x-prowsetk-opencode used: false) — see the note field in $YAML"
fi

if [[ "$KEEP_SERVER" -eq 1 && -n "$SERVER_PID" ]]; then
    log "keeping opencode server (pid $SERVER_PID, log $SERVER_LOG)"
fi
