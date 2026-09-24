#!/usr/bin/env python3
"""Filter scrape-endpoints OpenAPI YAML by confidence score.

Takes OpenAPI YAML emitted by the ``scrape-endpoints`` plugin (or the core
``EndpointExtractor``) and drops operations whose
``x-prowsetk-provenance.confidence`` is below a minimum threshold.

Confidence model (heuristic, never authoritative):
  * per-operation ``confidence: <float>`` under ``x-prowsetk-provenance``
  * operations without any confidence annotation are kept (nothing to judge)
  * paths left with zero operations are removed entirely
  * if no paths survive, the document keeps its header with ``paths: {}``

Usage:
    filter_openapi_yaml.py [INPUT] [OUTPUT] [--score MIN]
    filter_openapi_yaml.py --score 0.9 in.yaml -o out.yaml
    cat in.yaml | filter_openapi_yaml.py --score 0.9 > out.yaml

Only the standard library is used so the script runs anywhere python3 exists.
Formatting of surviving blocks is preserved verbatim (line-based filter, no
YAML round-trip).
"""

from __future__ import annotations

import argparse
import re
import sys

DEFAULT_MIN_SCORE = 0.80

HTTP_METHODS = frozenset(
    {"get", "post", "put", "patch", "delete", "head", "options", "trace"}
)

_TOP_PATHS_RE = re.compile(r"^paths\s*:(.*)$")
_PATH_KEY_RE = re.compile(r"^\s{2}\S.*:\s*(?:#.*)?$")
_OP_KEY_RE = re.compile(r"^\s{4}([A-Za-z][A-Za-z0-9_-]*)\s*:(.*)$")
_CONFIDENCE_RE = re.compile(r"^\s*confidence\s*:(.*)$")
_TOP_KEY_RE = re.compile(r"^\S.*:\s*(.*)$")


def indent_of(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def parse_score(raw: str) -> float | None:
    """Parse a confidence scalar; return None when unparseable."""
    text = raw.strip()
    if not text:
        return None
    # Strip trailing YAML comment (confidence is numeric; '#' always starts one).
    if "#" in text:
        text = text.split("#", 1)[0].strip()
    # Strip surrounding single/double quotes.
    if len(text) >= 2 and text[0] == text[-1] and text[0] in ("'", '"'):
        text = text[1:-1].strip()
        if text.startswith("'") or text.startswith('"'):
            return None
    try:
        value = float(text)
    except ValueError:
        return None
    return value


def operation_confidence(op_lines: list[str]) -> float | None:
    """Return the first parseable ``confidence:`` value in an operation block."""
    for line in op_lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        match = _CONFIDENCE_RE.match(line)
        if not match:
            continue
        value = parse_score(match.group(1))
        if value is not None:
            return value
    return None


def split_path_block(
    block: list[str],
) -> tuple[str, list[tuple[str, str, list[str]]]]:
    """Split a path block into (path_header, [(name, header, body)]) chunks.

    The first element is the ``  '/path':`` header line. Each chunk is a
    4-space-indented sub-block (an HTTP operation or a shared key such as
    path-level ``parameters:``). Continuation lines (indent > 4, blanks,
    comments) belong to the open chunk.
    """
    header = block[0]
    chunks: list[tuple[str, str, list[str]]] = []
    current_name: str | None = None
    current_header: str | None = None
    current_body: list[str] = []
    for line in block[1:]:
        match = _OP_KEY_RE.match(line)
        if match is not None:
            if current_name is not None and current_header is not None:
                chunks.append((current_name, current_header, current_body))
            current_name = match.group(1)
            current_header = line
            current_body = []
        else:
            if current_name is None:
                # Stray line directly under the path key (unexpected shape);
                # keep it attached as a non-operation chunk so it survives
                # whenever the path survives.
                current_name = ""
                current_header = line
                current_body = []
                chunks.append((current_name, current_header, current_body))
                current_name = None
                current_header = None
                current_body = []
            else:
                current_body.append(line)
    if current_name is not None and current_header is not None:
        chunks.append((current_name, current_header, current_body))
    return header, chunks


def filter_document(text: str, minimum: float) -> tuple[str, dict[str, int]]:
    stats = {
        "paths_kept": 0,
        "paths_removed": 0,
        "ops_kept": 0,
        "ops_removed": 0,
    }
    lines = text.splitlines()

    paths_idx: int | None = None
    paths_inline: str | None = None
    for i, line in enumerate(lines):
        if line.strip() == "" or line.lstrip().startswith("#"):
            continue
        if indent_of(line) != 0:
            continue
        match = _TOP_PATHS_RE.match(line.strip())
        if match:
            paths_idx = i
            paths_inline = match.group(1).strip()
            break
    if paths_idx is None:
        raise ValueError("no top-level 'paths:' mapping found in input")

    header = lines[: paths_idx + 1]

    # `paths: {}` (or `[]`) — nothing to filter; pass through normalized.
    if paths_inline:
        code = paths_inline.split("#", 1)[0].strip()
        if code in ("{}", "{ }", "[]", "[ ]"):
            return "\n".join(lines) + ("\n" if lines else ""), stats

    # Split remainder into paths-section vs footer (next indent-0 key).
    footer_idx: int | None = None
    for i in range(paths_idx + 1, len(lines)):
        line = lines[i]
        if line.strip() == "" or line.lstrip().startswith("#"):
            continue
        if indent_of(line) == 0 and _TOP_KEY_RE.match(line):
            footer_idx = i
            break
    if footer_idx is None:
        section = lines[paths_idx + 1 :]
        footer: list[str] = []
    else:
        section = lines[paths_idx + 1 : footer_idx]
        footer = lines[footer_idx:]

    # Group section lines into path blocks (each starts at exactly 2 spaces).
    path_blocks: list[list[str]] = []
    pending_blank: list[str] = []
    for line in section:
        if line.strip() == "":
            pending_blank.append(line)
            continue
        if indent_of(line) == 2 and _PATH_KEY_RE.match(line):
            block = pending_blank
            pending_blank = []
            block = list(block)
            path_blocks.append(block)
            path_blocks[-1].append(line)
        elif path_blocks:
            if pending_blank:
                path_blocks[-1].extend(pending_blank)
                pending_blank = []
            path_blocks[-1].append(line)
        else:
            # Leading stray lines (comments/blanks already handled; anything
            # else is unexpected) — fold into header so it is preserved.
            if pending_blank:
                header.extend(pending_blank)
                pending_blank = []
            header.append(line)
    # Trailing blanks inside the section are insignificant; drop them so an
    # emptied `paths:` does not leave stray whitespace. Blanks in the footer
    # are preserved verbatim.
    pending_blank = []

    out: list[str] = list(header)
    kept_any_path = False
    for block in path_blocks:
        # Skip blocks that are only blank lines (should not happen).
        significant = [ln for ln in block if ln.strip() != ""]
        if not significant:
            continue
        path_header, chunks = split_path_block(significant)
        kept_chunks: list[list[str]] = []
        for name, chunk_header, chunk_body in chunks:
            if name.lower() not in HTTP_METHODS:
                kept_chunks.append([chunk_header] + chunk_body)
                continue
            confidence = operation_confidence([chunk_header] + chunk_body)
            if confidence is None or confidence >= minimum:
                kept_chunks.append([chunk_header] + chunk_body)
                stats["ops_kept"] += 1
            else:
                stats["ops_removed"] += 1
        if kept_chunks:
            out.append(path_header)
            for chunk in kept_chunks:
                out.extend(chunk)
            stats["paths_kept"] += 1
            kept_any_path = True
        else:
            # Path had only filterable operations and all were dropped
            # (or was empty) — remove the whole path.
            if any(name.lower() in HTTP_METHODS for name, _, _ in chunks):
                stats["paths_removed"] += 1
            else:
                # Path with no operations at all: keep the header so the
                # output still reflects the input shape.
                out.append(path_header)
                stats["paths_kept"] += 1
                kept_any_path = True

    if not kept_any_path:
        # Mirror the emitter's empty shape: `paths: {}`.
        out = list(header)
        out[-1] = "paths: {}"
    out.extend(footer)
    result = "\n".join(out)
    if result and not result.endswith("\n"):
        result += "\n"
    return result, stats


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Filter scrape-endpoints OpenAPI YAML by minimum confidence. "
            "Operations with x-prowsetk-provenance.confidence below the "
            "threshold are dropped; operations without a confidence value "
            "are kept. Reads stdin and writes stdout by default."
        ),
    )
    parser.add_argument(
        "input",
        nargs="?",
        default=None,
        help="input YAML file ('-' or omitted reads stdin)",
    )
    parser.add_argument(
        "output",
        nargs="?",
        default=None,
        help="output YAML file ('-' or omitted writes stdout; "
        "--output takes precedence)",
    )
    parser.add_argument(
        "-i",
        "--input",
        dest="input_opt",
        default=None,
        help="input YAML file (overrides positional INPUT)",
    )
    parser.add_argument(
        "-o",
        "--output",
        dest="output_opt",
        default=None,
        help="output YAML file (overrides positional OUTPUT)",
    )
    parser.add_argument(
        "-S",
        "--score",
        dest="score",
        type=float,
        default=DEFAULT_MIN_SCORE,
        metavar="MIN",
        help="minimum confidence to keep (0.0-1.0, default %(default).2f)",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="suppress the keep/remove summary on stderr",
    )
    return parser


def read_input(path: str | None) -> str:
    if path is None or path == "-":
        return sys.stdin.read()
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def write_output(path: str | None, payload: str) -> None:
    if path is None or path == "-":
        sys.stdout.write(payload)
        return
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(payload)


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    minimum = args.score
    if minimum != minimum or minimum < 0.0 or minimum > 1.0:  # NaN check
        parser.error("--score must be a number in the range 0.0-1.0")

    input_path = args.input_opt if args.input_opt is not None else args.input
    output_path = args.output_opt if args.output_opt is not None else args.output
    if input_path == "-":
        input_path = None
    if output_path == "-":
        output_path = None

    try:
        text = read_input(input_path)
    except OSError as exc:
        print(f"error: cannot read input: {exc}", file=sys.stderr)
        return 1
    if not text.strip():
        print("error: input is empty", file=sys.stderr)
        return 1

    try:
        filtered, stats = filter_document(text, minimum)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    try:
        write_output(output_path, filtered)
    except OSError as exc:
        print(f"error: cannot write output: {exc}", file=sys.stderr)
        return 1

    if not args.quiet:
        print(
            f"kept {stats['ops_kept']} operation(s) in "
            f"{stats['paths_kept']} path(s); removed "
            f"{stats['ops_removed']} operation(s), "
            f"{stats['paths_removed']} path(s) "
            f"(min confidence >= {minimum:.2f})",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
