#!/usr/bin/env bash
#
# launch-gui.sh — start the ProwseTk/Flatworm desktop inspector.
#
# Wraps the `ptk-basic-gui` executable from the optional FLTK frontend
# (plugins/basic-gui). It locates the binary across CMake presets, applies the
# matching runtime library path, and forwards page options.
#
# The inspector uses the owning Flatworm Session, so every request keeps the
# host's cookies, proxies, TLS verification, request/response hooks and
# redaction. Nothing is logged here beyond the launch command and stage names;
# page values and URLs are never echoed.
#
# Usage:
#   tools/launch-gui.sh [options]
#
# Options:
#   --url URL            start URL; navigates over the network (http/https)
#   --file PATH          offline HTML file; uses a memory transport that rejects
#                        every page request, including script navigation
#   --base-url URL       base URL for relative references in --file
#                        (default: https://offline.test/)
#   --proxy URL          HTTP(S)/SOCKS5 proxy for page requests
#   --opencode-url URL   OpenCode server override for both GUI integrations
#   --marionette PATH    trusted Lua policy script to run through OpenCode
#   --goal TEXT          override its OpenCode goal (forwarded to the GUI)
#   --no-javascript      load the document without page scripting
#   --preset NAME        CMake preset to search first (default: gui)
#   --bin PATH           explicit ptk-basic-gui executable
#   --print-bin          print the resolved executable and exit
#   --check              verify the executable and display, then exit
#   -h, --help           show this help and exit
#
# Environment:
#   PROWSETK_GUI_BIN     explicit ptk-basic-gui executable (default: the first
#                        match found among presets, then $PATH)
#   DISPLAY / WAYLAND_DISPLAY   the GUI target's display; required unless the
#                        binary reports that it was built without FLTK
#   HTTPS_PROXY / HTTP_PROXY / ALL_PROXY   proxies for HTTPS / HTTP / fallback
#   NO_PROXY / no_proxy    comma-separated exclusions for environment proxies
#
# Build the frontend first:
#   cmake --preset gui
#   cmake --build --preset gui -j4
#
# Offline inspection (no network at all):
#   tools/launch-gui.sh --file plugins/basic-gui/example.html
#
# Live browsing:
#   tools/launch-gui.sh --url https://example.com
#
# Unrecognized options and everything after `--` are passed to ptk-basic-gui
# unchanged, so the executable owns their diagnostics.

set -euo pipefail

PRESET=""
BIN="${PROWSETK_GUI_BIN:-}"
URL=""
FILE=""
BASE_URL=""
PROXY=""
NO_JAVASCRIPT=0
PRINT_BIN=0
CHECK_ONLY=0
PASSTHROUGH=()

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

log() {
    echo "launch-gui: $*" >&2
}

die() {
    echo "launch-gui: error: $*" >&2
    exit 1
}

debug_log() {
    if [[ "${PROWSETK_GUI_VERBOSE:-0}" -eq 1 ]]; then
        echo "launch-gui: debug: $*" >&2
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --url) URL="${2:?--url needs a value}"; shift 2 ;;
        --file) FILE="${2:?--file needs a value}"; shift 2 ;;
        --base-url|--base_url) BASE_URL="${2:?--base-url needs a value}"; shift 2 ;;
        --proxy) PROXY="${2:?--proxy needs a value}"; shift 2 ;;
        --no-javascript) NO_JAVASCRIPT=1; shift ;;
        --preset) PRESET="${2:?--preset needs a value}"; shift 2 ;;
        --bin) BIN="${2:?--bin needs a value}"; shift 2 ;;
        --print-bin) PRINT_BIN=1; shift ;;
        --check) CHECK_ONLY=1; shift ;;
        -v|--verbose) PROWSETK_GUI_VERBOSE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; PASSTHROUGH+=("$@"); break ;;
        *) PASSTHROUGH+=("$1"); shift ;;
    esac
done

if [[ -n "$URL" && -n "$FILE" ]]; then
    die "choose either --url or --file, not both"
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"

# Presets are searched in order: the caller's choice, then the GUI preset, then
# the ordinary development presets. --help works without any of them existing.
PRESET_ORDER=()
[[ -n "$PRESET" ]] && PRESET_ORDER+=("$PRESET")
PRESET_ORDER+=(gui default debug release)

find_binary() {
    local preset path
    for preset in "${PRESET_ORDER[@]}"; do
        [[ -n "$preset" ]] || continue
        for path in \
            "${REPO_ROOT}/build/${preset}/plugins/basic-gui/ptk-basic-gui" \
            "${REPO_ROOT}/build/${preset}/tools/basic-gui/ptk-basic-gui"
        do
            if [[ -x "$path" ]]; then
                printf '%s\n' "$path"
                return 0
            fi
        done
    done
    # An installed copy is a valid launch target too.
    if command -v ptk-basic-gui >/dev/null 2>&1; then
        command -v ptk-basic-gui
        return 0
    fi
    return 1
}

if [[ -z "$BIN" ]]; then
    BIN="$(find_binary || true)"
fi
if [[ -z "$BIN" ]]; then
    die "ptk-basic-gui not found in build/${PRESET_ORDER[*]}
   build the GUI frontend first:
     cmake --preset gui
     cmake --build --preset gui
   or pass --bin PATH / set PROWSETK_GUI_BIN"
fi
[[ -x "$BIN" ]] || die "not executable: $BIN"
# Never leak secret-bearing proxy credentials through process listings.
if [[ -n "$PROXY" ]]; then
    case "$PROXY" in
        *://*@*)
            die "--proxy must not embed credentials; use HTTPS_PROXY / HTTP_PROXY
     or a proxy URL without userinfo"
            ;;
    esac
fi
BIN="$(realpath "$BIN" 2>/dev/null || printf '%s' "$BIN")"
BIN_DIR="$(dirname -- "$BIN")"
debug_log "resolved executable: $BIN"

# The plugin shared library sits beside the executable; make it loadable without
# requiring the caller to know the build layout.
export LD_LIBRARY_PATH="${BIN_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
if [[ -n "${DYLD_LIBRARY_PATH:-}" ]]; then
    export DYLD_LIBRARY_PATH="${BIN_DIR}:${DYLD_LIBRARY_PATH}"
fi

if [[ "$PRINT_BIN" -eq 1 ]]; then
    printf '%s\n' "$BIN"
    exit 0
fi

# --print-bin and --check are diagnostics: they must work with no arguments so
# they can answer "is the GUI build usable?" without inventing a page.
if [[ "$CHECK_ONLY" -eq 0 ]]; then
    if [[ -n "$FILE" ]]; then
        [[ -r "$FILE" ]] || die "HTML file is not readable: $FILE"
    else
        [[ -n "$URL" ]] || die "nothing to open: pass --url URL or --file PATH (see --help)"
        case "$URL" in
            http://*|https://*) ;;
            *) die "--url requires an http:// or https:// URL" ;;
        esac
    fi

    # A headless display is a configuration error, not a silent empty window.
    # FLTK aborts on Linux without a display, so report it before the process
    # starts rather than surfacing it as a failed launch.
    if [[ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; then
        die "no display: set DISPLAY (or WAYLAND_DISPLAY), or run under Xvfb:
     xvfb-run -a tools/launch-gui.sh --file plugins/basic-gui/example.html"
    fi
fi

if [[ "$CHECK_ONLY" -eq 1 ]]; then
    if "$BIN" --help >/dev/null 2>&1; then
        log "ok: $BIN (display: ${DISPLAY:-${WAYLAND_DISPLAY:-unset}})"
        exit 0
    fi
    die "ptk-basic-gui --help failed for $BIN
   a GUI-disabled build reports Unsupported here; rebuild with --preset gui"
fi

ARGS=()
[[ -n "$URL" ]] && ARGS+=(--url "$URL")
[[ -n "$FILE" ]] && ARGS+=(--file "$FILE")
[[ -n "$BASE_URL" ]] && ARGS+=(--base-url "$BASE_URL")
[[ -n "$PROXY" ]] && ARGS+=(--proxy "$PROXY")
[[ "$NO_JAVASCRIPT" -eq 1 ]] && ARGS+=(--no-javascript)

# Proxy diagnostics report only whether a variable is set; no proxy or page
# values are printed.
if [[ "${PROWSETK_GUI_VERBOSE:-0}" -eq 1 ]]; then
    debug_log "proxy environment: HTTPS_PROXY=$([[ -n "${HTTPS_PROXY:-${https_proxy:-}}" ]] && echo set || echo unset), HTTP_PROXY=$([[ -n "${HTTP_PROXY:-${http_proxy:-}}" ]] && echo set || echo unset)"
    debug_log "forwarding arguments: ${#ARGS[@]} option(s) plus ${#PASSTHROUGH[@]} passthrough argument(s)"
fi

if [[ -n "$FILE" ]]; then
    log "opening offline document $FILE (page requests are blocked)"
elif [[ "$NO_JAVASCRIPT" -eq 1 ]]; then
    log "opening $URL without page scripting"
else
    log "opening $URL"
fi
debug_log "executing $BIN"

exec "$BIN" "${ARGS[@]}" ${PASSTHROUGH+"${PASSTHROUGH[@]}"}