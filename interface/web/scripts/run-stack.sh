#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"

if [[ ! -x "${ROOT_DIR}/.venv/bin/uvicorn" ]]; then
  echo "Missing .venv. Run scripts/install.sh first."
  exit 1
fi

PROWSETK_BIN="${PROWSETK_BIN:-${PROJECT_ROOT}/build/default/src/cli/prowsetk}"
if [[ ! -x "${PROWSETK_BIN}" ]]; then
  echo "ProwseTk binary not found at ${PROWSETK_BIN}"
  echo "Build first: cmake --build --preset default"
  exit 1
fi

cleanup() {
  if [[ -n "${UPSTREAM_PID:-}" ]]; then
    kill "${UPSTREAM_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

UPSTREAM_HOST="${PROWSETK_UPSTREAM_HOST:-127.0.0.1}"
UPSTREAM_PORT="${PROWSETK_UPSTREAM_PORT:-8080}"

"${PROWSETK_BIN}" serve --host "${UPSTREAM_HOST}" --port "${UPSTREAM_PORT}" &
UPSTREAM_PID=$!

PROWSETK_WEB_PROWSETK_UPSTREAM="http://${UPSTREAM_HOST}:${UPSTREAM_PORT}" \
  "${ROOT_DIR}/scripts/dev.sh"
