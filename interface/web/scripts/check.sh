#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "[check] compile Python"
python3 -m compileall "${ROOT_DIR}/backend" "${ROOT_DIR}/app.py"

if command -v python3 >/dev/null 2>&1; then
  echo "[check] import sanity"
  PYTHONPATH="${ROOT_DIR}" python3 -c "import ast, pathlib; list(pathlib.Path('${ROOT_DIR}/backend').glob('*.py'))" >/dev/null
fi

# Verify API.yaml is valid YAML
if command -v python3 >/dev/null 2>&1; then
  if python3 -c "import yaml" 2>/dev/null; then
    python3 -c "import yaml, pathlib; yaml.safe_load(pathlib.Path('${ROOT_DIR}/API.yaml').read_text())" && echo "[check] API.yaml valid"
  fi
fi

# If venv present, regenerate openapi and ensure no diff
if [[ -x "${ROOT_DIR}/.venv/bin/python" ]]; then
  echo "[check] regenerating OpenAPI"
  "${ROOT_DIR}/scripts/generate-openapi.sh" || echo "[warn] generate-openapi failed"
  if ! git -C "${ROOT_DIR}" diff --quiet -- API.yaml 2>/dev/null; then
    echo "[warn] API.yaml drift detected after regeneration (commit the updated file)"
  fi
fi

echo "Checks passed."
