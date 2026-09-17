#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 -m compileall "${ROOT_DIR}/backend" "${ROOT_DIR}/app.py"

if [[ -x "${ROOT_DIR}/.venv/bin/python" ]]; then
  "${ROOT_DIR}/scripts/generate-openapi.sh"
fi

echo "Checks passed."
