from __future__ import annotations

from pathlib import Path

import yaml

from backend.main import create_app


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    out = root / "API.yaml"
    schema = create_app().openapi()
    out.write_text(yaml.safe_dump(schema, sort_keys=False), encoding="utf-8")
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
