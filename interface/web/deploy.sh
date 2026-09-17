#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"${ROOT_DIR}/scripts/install.sh"
"${ROOT_DIR}/scripts/generate-openapi.sh"

exec "${ROOT_DIR}/scripts/dev.sh"
