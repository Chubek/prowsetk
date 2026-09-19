#!/usr/bin/env python3
"""
booking_dotcom_endpoints.py — Login to Booking.com admin panel and scrape OpenAPI.

Loads credentials from environment (via python-dotenv), authenticates a
ProwseTk session, crawls the panel, discovers endpoints and writes an
OpenAPI 3.x YAML file.

Environment variables (loaded via ``dotenv.load_dotenv()``):
    BOOKING_DOTCOM_URL   URL of the login panel (e.g. https://admin.booking.com)
    BOOKING_DOTCOM_USER  username / email
    BOOKING_DOTCOM_PASS  password

CLI usage:
    python -m pyprowsetk.examples.booking_dotcom_endpoints --help
    python interface/pyprowsetk/examples/booking_dotcom_endpoints.py --output ~/BookingDotcomAPI.yaml
    BOOKING_DOTCOM_URL=https://admin.booking.com BOOKING_DOTCOM_USER=... \\
        BOOKING_DOTCOM_PASS=... python interface/pyprowsetk/examples/booking_dotcom_endpoints.py

Output defaults to ``$HOME/BookingDotcomAPI.yaml`` and can be overridden
with ``-o / --output``.
"""

from __future__ import annotations

import argparse
import os
import sys
import urllib.parse
from pathlib import Path

# ---------------------------------------------------------------------------
# 1. Load environment variables via dotenv — must happen before any os.getenv
# ---------------------------------------------------------------------------
from dotenv import load_dotenv

load_dotenv()  # loads .env from cwd and parent dirs; no-op if absent

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _normalize_url(url: str) -> str:
    url = url.strip()
    if not url:
        return url
    if not url.startswith("http://") and not url.startswith("https://"):
        # Booking panel is always https; be permissive for bare hostnames.
        url = "https://" + url.lstrip("/")
    return url.rstrip("/")


def _resolve_url(base: str, reference: str) -> str:
    if not reference:
        return base
    return urllib.parse.urljoin(base + "/", reference)


def _env(name: str, fallback_names: list[str] | None = None) -> str:
    """Fetch env var with optional fallback aliases (handles historic typo)."""
    val = os.getenv(name)
    if val:
        return val
    if fallback_names:
        for alt in fallback_names:
            v = os.getenv(alt)
            if v:
                return v
    return ""


def _looks_like_username(name: str) -> bool:
    n = name.lower()
    for needle in ("user", "email", "login", "account", "name"):
        if needle in n:
            return True
    return False


def _find_login_form(document):
    """Return (form, username_input, password_input) or (None, None, None).

    Heuristic mirrors drivers/login.lua: pick the first form containing a
    password field and the best username candidate.
    """
    try:
        forms = document.query_selector_all("form")
    except Exception:
        forms = []
    for form in forms:
        password = None
        username = None
        fallback = None
        try:
            inputs = form.query_selector_all("input")
        except Exception:
            inputs = []
        for inp in inputs:
            try:
                itype = (inp.attribute("type") or "").lower()
                iname = (inp.attribute("name") or "").lower()
            except Exception:
                continue
            if itype == "password" and password is None:
                password = inp
            elif itype in ("text", "email", ""):
                if fallback is None:
                    fallback = inp
                if username is None and _looks_like_username(iname):
                    username = inp
        if password is not None:
            return form, (username or fallback), password
    return None, None, None


def _collect_form_fields(form):
    """Collect all inputs with a name attribute as {name: value} list."""
    fields: list[dict] = []
    try:
        inputs = form.query_selector_all("input")
    except Exception:
        return fields
    for inp in inputs:
        try:
            name = inp.attribute("name")
        except Exception:
            continue
        if not name:
            continue
        try:
            value = inp.value() or inp.attribute("value") or ""
        except Exception:
            value = ""
        fields.append({"name": name, "value": value, "element": inp})
    return fields


def _login(session, username: str, password: str) -> dict:
    """Fill and submit the login form in the current session document.

    Returns a summary dict (no secrets). Raises on failure.
    """
    doc = session.document()
    if doc is None or not doc.valid():
        raise RuntimeError("no document loaded — cannot find login form")

    form, user_input, pass_input = _find_login_form(doc)
    if form is None or pass_input is None:
        raise RuntimeError("no password field found — login form not detected")
    if user_input is None:
        raise RuntimeError("no username field found — login form not detected")

    # Fill credentials in DOM (validates set_value round-trip)
    user_input.set_value(username)
    pass_input.set_value(password)
    # also patch the field list values for submission
    # so hidden / other inputs keep original values
    if user_input.value() != username or pass_input.value() != password:
        raise RuntimeError("failed to populate credential fields")

    method = (form.attribute("method") or "post").lower()
    if method not in ("get", "post"):
        method = "post"
    action_raw = form.attribute("action") or ""
    # session.current_url is a property (str)
    try:
        current = session.current_url or ""
    except Exception:
        current = ""
    action = _resolve_url(current, action_raw)

    fields = _collect_form_fields(form)
    # Ensure username/password values are present under correct field names
    # (collect already did set_value, but ensure the dict reflects it)
    user_field_name = user_input.attribute("name")
    pass_field_name = pass_input.attribute("name")
    for f in fields:
        if f["name"] == user_field_name:
            f["value"] = username
        if f["name"] == pass_field_name:
            f["value"] = password

    # Build body
    body = urllib.parse.urlencode({f["name"]: f["value"] for f in fields})

    # Lazy import to avoid hard dependency at module import time
    import pyprowsetk as pk

    req = pk.HttpRequest()
    req.method = method.upper()
    req.url = action
    req.headers = [("Content-Type", "application/x-www-form-urlencoded")]
    # Keep cookies from the GET — Browser's storage handles it
    if method == "get":
        # encode as query string instead of body
        sep = "&" if "?" in action else "?"
        req.url = action + sep + body
        req.body = ""
    else:
        req.body = body

    resp = session.request(req)

    if resp.status >= 400:
        raise RuntimeError(f"authentication request failed with status {resp.status}")

    return {
        "action": action,
        "method": method.upper(),
        "username_field": user_field_name,
        "password_field": pass_field_name,
        "status": resp.status,
        "final_url": resp.final_url or action,
    }


def _crawl_and_extract(browser, start_url: str, max_depth: int, max_pages: int,
                       verbose: bool = False, scrape_all: bool = True,
                       min_confidence: float = 0.5):
    """Crawl same-origin links from start_url and extract endpoints.

    Reuses the authenticated Browser so cookies are shared across sessions.
    Returns (endpoints, warnings, visited_urls).
    """
    import pyprowsetk as pk

    extractor = pk.EndpointExtractor()
    # Configure extraction options for breadth — comprehensive by default
    # so the Booking panel scrape captures *every* href/resource endpoint,
    # not just API-like paths. This drives the C++ scrape_all_paths flag.
    try:
        opts = extractor.options
        opts.follow_links = True
        opts.inspect_scripts = True
        opts.observe_network = False
        opts.infer_schemas = True
        opts.include_provenance = True
        opts.redact_secrets = True
        opts.max_depth = max_depth
        opts.max_pages = max_pages
        opts.minimum_confidence = float(min_confidence)
        opts.openapi_version = "3.1.0"
        # Comprehensive scrape: include all paths (non-API anchors etc.)
        if hasattr(opts, "scrape_all_paths"):
            opts.scrape_all_paths = bool(scrape_all)
    except Exception:
        pass

    # Determine origin for same-origin filter
    try:
        origin_host = pk.parse_url(start_url).host
    except Exception:
        origin_host = ""

    visited: set[str] = set()
    queue: list[tuple[str, int]] = [(start_url, 0)]
    all_endpoints: list = []
    all_warnings: list[str] = []
    seen_keys: set[str] = set()

    while queue and len(visited) < max_pages:
        url, depth = queue.pop(0)
        if url in visited:
            continue
        visited.add(url)

        sess = browser.create_session()
        try:
            if verbose:
                print(f"[crawl] depth={depth} {url}", file=sys.stderr)
            sess.navigate(url)
        except Exception as exc:
            if verbose:
                print(f"[crawl] navigate failed {url}: {exc}", file=sys.stderr)
            try:
                sess.close()
            except Exception:
                pass
            continue

        doc = sess.document()
        if doc is None or not doc.valid():
            try:
                sess.close()
            except Exception:
                pass
            continue

        # Extract endpoints from this document
        try:
            result = extractor.extract(doc)
            for ep in result.endpoints:
                key = f"{ep.method} {ep.path}"
                if key not in seen_keys:
                    seen_keys.add(key)
                    all_endpoints.append(ep)
            for w in result.warnings:
                if w not in all_warnings:
                    all_warnings.append(w)
        except Exception as exc:
            if verbose:
                print(f"[extract] failed on {url}: {exc}", file=sys.stderr)

        # Enqueue same-origin links for next depth
        if depth < max_depth and len(visited) + len(queue) < max_pages:
            try:
                links = doc.links()
            except Exception:
                links = []
            for link_el in links:
                try:
                    href = link_el.attribute("href")
                except Exception:
                    continue
                if not href or href.startswith("#") or href.startswith("javascript:"):
                    continue
                try:
                    abs_url = pk.resolve_url(sess.current_url or url, href)
                except Exception:
                    continue
                # same-origin filter
                try:
                    host = pk.parse_url(abs_url).host
                except Exception:
                    continue
                if origin_host and host != origin_host:
                    continue
                # strip fragment
                abs_url = abs_url.split("#")[0]
                if abs_url not in visited and abs_url not in [q[0] for q in queue]:
                    queue.append((abs_url, depth + 1))

        try:
            sess.close()
        except Exception:
            pass

    return all_endpoints, all_warnings, visited


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Login to Booking.com panel and scrape OpenAPI endpoints.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    default_output = str(Path.home() / "BookingDotcomAPI.yaml")
    parser.add_argument(
        "-o", "--output",
        default=default_output,
        help="Output path for OpenAPI YAML (default: $HOME/BookingDotcomAPI.yaml)",
    )
    parser.add_argument(
        "--url",
        default=None,
        help="Panel URL (default: $BOOKING_DOTCOM_URL from env/.env)",
    )
    parser.add_argument(
        "--username",
        default=None,
        help="Username (default: $BOOKING_DOTCOM_USER from env/.env)",
    )
    parser.add_argument(
        "--password",
        default=None,
        help="Password (default: $BOOKING_DOTCOM_PASS from env/.env)",
    )
    parser.add_argument(
        "--max-depth",
        type=int,
        default=2,
        help="Maximum crawl depth from the start URL",
    )
    parser.add_argument(
        "--max-pages",
        type=int,
        default=100,
        help="Maximum number of pages to visit",
    )
    parser.add_argument(
        "--no-crawl",
        action="store_true",
        help="Only extract from the landing page after login (no link following)",
    )
    parser.add_argument(
        "--javascript",
        action="store_true",
        default=False,
        help="Enable JavaScript execution (requires QuickJS)",
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Verbose progress output",
    )
    parser.add_argument(
        "--html",
        default=None,
        help="Optional HTML string/file for offline deterministic run (loads via session:load_html instead of network)",
    )
    parser.add_argument(
        "--api-only",
        action="store_true",
        help="Only emit API-like paths (default is comprehensive: all hrefs/resources)",
    )
    parser.add_argument(
        "--min-confidence",
        type=float,
        default=0.5,
        help="Minimum confidence threshold (default 0.5; lower includes more low-confidence links)",
    )
    args = parser.parse_args(argv)

    # Resolve credentials: CLI > env (.env already loaded via load_dotenv)
    url = args.url or _env("BOOKING_DOTCOM_URL", fallback_names=["BOOKING_DOCTOM_URL"])
    username = args.username or _env("BOOKING_DOTCOM_USER")
    password = args.password or _env("BOOKING_DOTCOM_PASS")

    if not url:
        print("error: BOOKING_DOTCOM_URL is not set (and --url not provided).", file=sys.stderr)
        print("       Set it in .env or environment, e.g. BOOKING_DOTCOM_URL=https://admin.booking.com", file=sys.stderr)
        return 2
    if not username:
        print("error: BOOKING_DOTCOM_USER is not set (and --username not provided).", file=sys.stderr)
        return 2
    if not password:
        print("error: BOOKING_DOTCOM_PASS is not set (and --password not provided).", file=sys.stderr)
        return 2

    url = _normalize_url(url)
    output_path = Path(os.path.expanduser(os.path.expandvars(args.output))).resolve()
    # Honour $HOME convention explicitly for display
    if verbose_in_args := args.verbose:
        print(f"[info] panel URL: {url}", file=sys.stderr)
        print(f"[info] output: {output_path}", file=sys.stderr)

    # ------------------------------------------------------------------
    # Browser / Session — login
    # ------------------------------------------------------------------
    try:
        import pyprowsetk as pk
    except ImportError as exc:
        print(f"error: pyprowsetk not available: {exc}", file=sys.stderr)
        print("       Build with: cmake --preset default && cmake --build --preset default", file=sys.stderr)
        return 1

    cfg = pk.BrowserConfig()
    cfg.user_agent = "pyprowsetk-booking-endpoints/0.1"
    cfg.javascript = bool(args.javascript)
    cfg.follow_redirects = True
    cfg.observe_network = False
    cfg.timeout_ms = 30000

    browser = pk.Browser(cfg)

    # Initial navigation to login page (with http fallback for plain-http builds)
    # If --html is supplied, use offline load_html (deterministic, no network).
    sess = browser.create_session()
    navigated_url = url
    html_source: str | None = None
    if args.html is not None:
        # --html may be a file path or raw HTML
        p = Path(args.html)
        if p.is_file():
            html_source = p.read_text(encoding="utf-8")
        else:
            html_source = args.html
        try:
            if args.verbose:
                print(f"[info] loading offline HTML ({len(html_source)} bytes) for {url}", file=sys.stderr)
            sess.load_html(html_source, url)
        except Exception as exc:
            print(f"error: failed to load_html: {exc}", file=sys.stderr)
            try:
                sess.close()
            except Exception:
                pass
            return 1
    else:
        try:
            if args.verbose:
                print(f"[info] navigating to {url}", file=sys.stderr)
            sess.navigate(url)
        except Exception as exc:
            msg = str(exc)
            # Fallback: some builds only support plain http (no TLS)
            if url.startswith("https://") and "plain HTTP only" in msg:
                http_url = "http://" + url[len("https://"):]
                if args.verbose:
                    print(f"[warn] https failed ({msg}), retrying {http_url}", file=sys.stderr)
                try:
                    sess.navigate(http_url)
                    navigated_url = http_url
                except Exception as exc2:
                    print(f"error: failed to navigate to {url} (and {http_url}): {exc2}", file=sys.stderr)
                    try:
                        sess.close()
                    except Exception:
                        pass
                    return 1
            else:
                print(f"error: failed to navigate to {url}: {exc}", file=sys.stderr)
                try:
                    sess.close()
                except Exception:
                    pass
                return 1
        # keep effective URL for crawling
        if navigated_url != url:
            url = navigated_url

    # Offline deterministic path: if --html was supplied, extract directly
    # from the loaded document (dry-run, no network follow-up).
    if html_source is not None:
        try:
            doc = sess.document()
            if doc is not None and doc.valid():
                # Fill credentials in DOM for completeness (dry-run, never sent)
                try:
                    _find_login_form(doc)  # warm cache
                    form, user_inp, pass_inp = _find_login_form(doc)
                    if user_inp is not None and pass_inp is not None:
                        user_inp.set_value(username)
                        pass_inp.set_value(password)
                except Exception:
                    pass
            # Direct extraction from the single offline document — comprehensive
            extractor = pk.EndpointExtractor()
            try:
                opts = extractor.options
                opts.include_provenance = True
                opts.redact_secrets = True
                if hasattr(opts, "scrape_all_paths"):
                    opts.scrape_all_paths = not bool(args.api_only)
                if hasattr(opts, "minimum_confidence"):
                    opts.minimum_confidence = float(args.min_confidence)
            except Exception:
                pass
            result = extractor.extract(doc) if doc is not None and doc.valid() else pk.EndpointExtractor().extract(pk.open_html(html_source, url=url))
            endpoints = list(result.endpoints)
            warnings = list(result.warnings)
            yaml_text = result.openapi_yaml
            # Ensure output parent exists and write file
            output_path.parent.mkdir(parents=True, exist_ok=True)
            if not yaml_text or "openapi" not in yaml_text.lower():
                yaml_text = (
                    'openapi: 3.1.0\n'
                    'info:\n'
                    '  title: Booking.com Panel API\n'
                    '  version: 1.0.0\n'
                    f'  description: Auto-discovered from {url} (offline dry run)\n'
                    'paths: {}\n'
                )
            output_path.write_text(yaml_text, encoding="utf-8")
            print(f"Wrote {len(endpoints)} endpoint(s) from 1 page(s) to {output_path} (offline)")
            try:
                sess.close()
            except Exception:
                pass
            return 0
        except Exception as exc:
            print(f"error: offline extraction failed: {exc}", file=sys.stderr)
            try:
                sess.close()
            except Exception:
                pass
            return 1

    # Attempt login (online path)
    try:
        info = _login(sess, username, password)
        if args.verbose:
            print(f"[info] login submitted: {info['method']} {info['action']} -> {info['status']}", file=sys.stderr)
    except Exception as exc:
        # If login form not found, we may already be on an authenticated page
        # or the panel uses a different auth mechanism — continue to extraction
        # but warn the user.
        if args.verbose:
            print(f"[warn] login step failed/ skipped: {exc}", file=sys.stderr)
        info = None

    # Capture post-login URL as crawl start
    try:
        start_url = sess.current_url or url
    except Exception:
        start_url = url

    # Close the login session — subsequent crawl shares Browser cookies
    try:
        sess.close()
    except Exception:
        pass

    # ------------------------------------------------------------------
    # Endpoint discovery (online crawl)
    # ------------------------------------------------------------------
    if args.no_crawl:
        max_depth = 0
        max_pages = 1
    else:
        max_depth = max(0, args.max_depth)
        max_pages = max(1, args.max_pages)

    endpoints, warnings, visited = _crawl_and_extract(
        browser, start_url, max_depth=max_depth, max_pages=max_pages,
        verbose=args.verbose, scrape_all=not bool(args.api_only),
        min_confidence=float(args.min_confidence),
    )

    # Render final OpenAPI YAML
    if endpoints:
        # Use shared redactor for consistent secret handling
        try:
            redactor = pk.Redactor()
        except Exception:
            redactor = None
        opts = pk.EndpointExtractionOptions()
        opts.openapi_version = "3.1.0"
        opts.include_provenance = True
        opts.redact_secrets = True
        try:
            yaml_text = pk.render_openapi_yaml(endpoints, opts, redactor) if redactor is not None else None
        except Exception:
            yaml_text = None
        if not yaml_text:
            # Fallback: use first extraction's yaml if render fails
            doc = pk.open_html(f"<html><body>Crawled {len(visited)} pages</body></html>")
            fallback = pk.EndpointExtractor().extract(doc)
            yaml_text = fallback.openapi_yaml
    else:
        # No endpoints discovered — still emit a valid minimal spec
        # so downstream tooling has a well-formed file.
        tmp_doc = pk.open_html(
            f'<html><body><a href="{start_url}">start</a></body></html>',
            url=start_url,
        )
        tmp_res = pk.EndpointExtractor().extract(tmp_doc)
        yaml_text = tmp_res.openapi_yaml
        if not yaml_text or "openapi" not in yaml_text.lower():
            yaml_text = (
                'openapi: 3.1.0\n'
                'info:\n'
                '  title: Booking.com Panel API\n'
                f'  version: 1.0.0\n'
                f'  description: Auto-discovered from {start_url} (no endpoints inferred)\n'
                'paths: {}\n'
            )
        warnings.append("no endpoints discovered — emitted minimal spec")

    # Ensure parent directory exists
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(yaml_text, encoding="utf-8")

    if warnings and args.verbose:
        for w in warnings:
            print(f"[warn] {w}", file=sys.stderr)

    print(f"Wrote {len(endpoints)} endpoint(s) from {len(visited)} page(s) to {output_path}")
    if endpoints and args.verbose:
        for ep in endpoints[:20]:
            print(f"  {ep.method} {ep.path}  ({ep.discovery_method}, conf={ep.confidence:.2f})", file=sys.stderr)
        if len(endpoints) > 20:
            print(f"  ... and {len(endpoints) - 20} more", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
