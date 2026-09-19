"""
pyprowsetk - Python interface for ProwseTk

Wraps the full functionality of ProwseTk (Browser, Session, Document, Element,
NetworkClient, JavaScriptRuntime, LuaRuntime, Storage, Events, Endpoint
Extraction, IR, Plugins, WASM, ProjectConfig, WebInterface, etc.) and adds
Python-exclusive conveniences.

All C++ features are accessible via `pyprowsetk._core` (nanobind extension).
This high-level wrapper re-exports them and adds:

  - Context managers for Browser/Session
  - Pythonic query helpers and iteration
  - crawl() helper for same-origin crawling without writing Lua
  - extract_table() for HTML table -> list[dict]
  - DataFrame export (optional pandas)
  - Python plugin hook decorator
  - High-level Browser helper with ergonomic defaults
"""

from __future__ import annotations

import re
import json
from typing import Any, Callable, Dict, Iterable, List, Optional, Tuple

try:
    from . import _core as _c
except ImportError as e:
    # When developing without building, provide helpful error
    raise ImportError(
        "pyprowsetk._core extension not built. "
        "Configure with cmake --preset default and build with cmake --build --preset default"
    ) from e

# ---------------------------------------------------------------------------
# Re-export everything from _core for full ProwseTk feature access
# ---------------------------------------------------------------------------
from ._core import *  # noqa: F401,F403

# Explicit re-exports for IDE/lint friendliness
Browser = _c.Browser
Session = _c.Session
Document = _c.Document
Element = _c.Element
BrowserConfig = _c.BrowserConfig
SessionConfig = _c.SessionConfig
MemoryNetworkClient = _c.MemoryNetworkClient
HttpRequest = _c.HttpRequest
HttpResponse = _c.HttpResponse
EndpointExtractor = _c.EndpointExtractor
EndpointExtractionOptions = _c.EndpointExtractionOptions
WebInterface = _c.WebInterface
WebRequest = _c.WebRequest
WebResponse = _c.WebResponse

__all__ = [x for x in dir(_c) if not x.startswith("_")]
__all__ += ["crawl", "extract_table", "extract_tables", "document_to_dict", "element_to_dict",
            "open_html", "create_browser", "python_hooks"]

__version__ = getattr(_c, "VERSION", "0.1.0")

# ---------------------------------------------------------------------------
# Python-exclusive: fix context managers (nanobind __exit__ with None)
# ---------------------------------------------------------------------------
def _browser_enter(self):
    return self
def _browser_exit(self, exc_type, exc_val, exc_tb):
    return False
def _session_enter(self):
    return self
def _session_exit(self, exc_type, exc_val, exc_tb):
    try:
        self.close()
    except Exception:
        pass
    return False

try:
    _c.Browser.__enter__ = _browser_enter  # type: ignore
    _c.Browser.__exit__ = _browser_exit  # type: ignore
    _c.Session.__enter__ = _session_enter  # type: ignore
    _c.Session.__exit__ = _session_exit  # type: ignore
except Exception:
    pass

# ---------------------------------------------------------------------------
# Python-exclusive: ergonomic Browser factory
# ---------------------------------------------------------------------------

def create_browser(
    user_agent: str = "pyprowsetk/0.1",
    javascript: bool = False,
    follow_redirects: bool = True,
    observe_network: bool = False,
    timeout_ms: int = 30000,
    **kwargs: Any,
) -> Browser:
    """Create a Browser with Python-friendly defaults.

    Python-exclusive convenience over constructing BrowserConfig manually.
    """
    cfg = BrowserConfig()
    cfg.user_agent = user_agent
    cfg.javascript = javascript
    cfg.follow_redirects = follow_redirects
    cfg.observe_network = observe_network
    cfg.timeout_ms = timeout_ms
    for k, v in kwargs.items():
        if hasattr(cfg, k):
            setattr(cfg, k, v)
    return Browser(cfg)


def open_html(html: str, url: str = "http://localhost/", base_url: str = "") -> Document:
    """Python-exclusive: parse HTML string directly to Document without a Session."""
    return _c.parse_html(html, url, base_url)


# ---------------------------------------------------------------------------
# Python-exclusive: Document/Element helpers
# ---------------------------------------------------------------------------

def document_to_dict(doc: Document) -> Dict[str, Any]:
    """Python-exclusive: serialize Document to a JSON-serializable dict."""
    return {
        "url": doc.url(),
        "base_url": doc.base_url(),
        "title": doc.title(),
        "text": doc.text(),
        "html": doc.html(),
        "links": [e.attribute("href") for e in doc.links()],
        "forms": len(doc.forms()),
        "scripts": len(doc.scripts()),
        "resources": doc.resource_urls(),
        "metadata": doc.metadata(),
    }


def element_to_dict(el: Element) -> Dict[str, Any]:
    """Python-exclusive: serialize Element to dict."""
    return {
        "tag": el.tag_name(),
        "id": el.id(),
        "class": el.class_name(),
        "text": el.text(),
        "html": el.inner_html(),
        "attributes": {a.name: a.value for a in el.attributes()},
    }


def extract_table(element: Element) -> List[Dict[str, str]]:
    """Python-exclusive: extract a <table> Element into list of row dicts.

    Uses first row as header (th/td). Handles colspan-free tables.
    """
    rows = element.query_selector_all("tr")
    if not rows:
        return []
    # header
    header_cells = rows[0].query_selector_all("th")
    if not header_cells:
        header_cells = rows[0].query_selector_all("td")
    headers = [c.text().strip() for c in header_cells]
    data: List[Dict[str, str]] = []
    for row in rows[1:]:
        cells = row.query_selector_all("td")
        if not cells:
            cells = row.query_selector_all("th")
        values = [c.text().strip() for c in cells]
        # pad/truncate to header length
        if len(values) < len(headers):
            values += [""] * (len(headers) - len(values))
        elif len(values) > len(headers):
            values = values[: len(headers)]
        data.append({headers[i]: values[i] for i in range(len(headers))} if headers else {str(i): v for i, v in enumerate(values)})
    return data


def extract_tables(doc: Document) -> List[List[Dict[str, str]]]:
    """Python-exclusive: extract all tables in a Document."""
    return [extract_table(t) for t in doc.get_elements_by_tag_name("table")]


# ---------------------------------------------------------------------------
# Python-exclusive: crawl helper (no Lua needed)
# ---------------------------------------------------------------------------

def crawl(
    start_url: str,
    max_depth: int = 1,
    same_origin: bool = True,
    browser: Optional[Browser] = None,
    network_client: Optional[Any] = None,
) -> List[Dict[str, Any]]:
    """Python-exclusive: crawl same-origin links up to depth, returning page dicts.

    This is a pure-Python alternative to the Lua crawl-site driver. It uses
    Browser/Session and MemoryNetworkClient when offline, or real network when
    available. Each result contains url, title, text, links.
    """
    own_browser = False
    if browser is None:
        browser = create_browser()
        own_browser = True
        if network_client is not None:
            # caller supplied a pre-configured MemoryNetworkClient
            try:
                browser.set_network_client(network_client)
            except Exception:
                pass

    visited: set[str] = set()
    queue: List[Tuple[str, int]] = [(start_url, 0)]
    pages: List[Dict[str, Any]] = []
    start_origin = _c.parse_url(start_url).host if start_url else ""

    while queue:
        url, depth = queue.pop(0)
        if url in visited:
            continue
        visited.add(url)
        sess = browser.create_session()
        try:
            sess.navigate(url)
        except Exception:
            # try load_html fallback if url is actually html content?
            continue
        doc = sess.document()
        if doc is None or not doc.valid():
            continue
        pages.append({
            "url": sess.current_url,
            "title": doc.title(),
            "text": doc.text(),
            "html": doc.html(),
            "links": [e.attribute("href") for e in doc.links()],
            "depth": depth,
        })
        if depth < max_depth:
            for link_el in doc.links():
                href = link_el.attribute("href")
                if not href:
                    continue
                try:
                    abs_url = _c.resolve_url(sess.current_url or start_url, href)
                except Exception:
                    continue
                if same_origin:
                    try:
                        host = _c.parse_url(abs_url).host
                    except Exception:
                        continue
                    if host != start_origin:
                        continue
                if abs_url not in visited:
                    queue.append((abs_url, depth + 1))
        sess.close()

    return pages


# ---------------------------------------------------------------------------
# Python-exclusive: lightweight Python plugin hook registry
# ---------------------------------------------------------------------------

class _HookRegistry:
    """Python-exclusive: register Python callables as ProwseTk event handlers.

    Example:
        hooks = python_hooks(browser)
        @hooks.before_request
        def my_hook(event):
            event.attributes["x-added"] = "1"
    """
    def __init__(self, browser: Browser):
        self.browser = browser
        self._ids: List[int] = []

    def on(self, event_type: Any, handler: Callable) -> int:
        et = event_type
        if isinstance(event_type, str):
            parsed = _c.parse_event_type(event_type)
            if parsed is None:
                raise ValueError(f"unknown event type: {event_type}")
            et = parsed
        sid = self.browser.events().subscribe(et, handler)
        self._ids.append(sid)
        return sid

    def before_request(self, fn: Callable) -> Callable:
        self.on(_c.EventType.BeforeRequest, fn)
        return fn

    def after_response(self, fn: Callable) -> Callable:
        self.on(_c.EventType.AfterResponse, fn)
        return fn

    def before_navigation(self, fn: Callable) -> Callable:
        self.on(_c.EventType.BeforeNavigation, fn)
        return fn

    def after_navigation(self, fn: Callable) -> Callable:
        self.on(_c.EventType.AfterNavigation, fn)
        return fn

    def document_created(self, fn: Callable) -> Callable:
        self.on(_c.EventType.DocumentCreated, fn)
        return fn

    def clear(self):
        for sid in self._ids:
            try:
                self.browser.events().unsubscribe(sid)
            except Exception:
                pass
        self._ids.clear()


def python_hooks(browser: Browser) -> _HookRegistry:
    """Create a Python hook registry bound to a Browser's EventDispatcher."""
    return _HookRegistry(browser)


# ---------------------------------------------------------------------------
# Python-exclusive: pandas integration (optional, no hard dependency)
# ---------------------------------------------------------------------------

def to_dataframe(data: List[Dict[str, Any]]):
    """Python-exclusive: convert list of dicts (e.g. from extract_table) to DataFrame if pandas is available."""
    try:
        import pandas as pd  # type: ignore
    except ImportError as e:
        raise ImportError("pandas is required for to_dataframe; pip install pandas") from e
    return pd.DataFrame(data)
