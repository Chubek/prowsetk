from __future__ import annotations

import sys
from pathlib import Path

import yaml

from backend.main import create_app


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    out = root / "API.yaml"
    try:
        schema = create_app().openapi()
    except Exception as exc:
        print(f"Failed to generate OpenAPI: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
    # Ensure stable output: sort keys alphabetically per level is done by yaml; we keep original order.
    payload = yaml.safe_dump(schema, sort_keys=False, allow_unicode=False, width=100)
    if not payload.strip():
        print("Generated schema is empty", file=sys.stderr)
        raise SystemExit(1)
    out.write_text(payload, encoding="utf-8")
    print(f"Wrote {out} ({len(payload)} bytes)")


if __name__ == "__main__":
    main()
