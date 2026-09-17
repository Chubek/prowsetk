#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ ! -x "${ROOT_DIR}/.venv/bin/python" ]]; then
  echo "Missing .venv. Run scripts/install.sh first."
  exit 1
fi

"${ROOT_DIR}/.venv/bin/python" "${ROOT_DIR}/scripts/generate-openapi.py"
