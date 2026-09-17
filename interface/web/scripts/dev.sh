#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -x "${ROOT_DIR}/.venv/bin/uvicorn" ]]; then
  echo "Missing .venv. Run scripts/install.sh first."
  exit 1
fi

exec "${ROOT_DIR}/.venv/bin/uvicorn" \
  backend.main:app \
  --app-dir "${ROOT_DIR}" \
  --reload \
  --host "${PROWSETK_WEB_HOST:-127.0.0.1}" \
  --port "${PROWSETK_WEB_PORT:-8090}"
