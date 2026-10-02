# Chapter 28: Web and Python Interfaces

[Manual index](README.md)

## Core REST interface

`prowsetk serve --host 127.0.0.1 --port 8080` starts the core HTTP/JSON service
and static UI. `--web-root DIR` selects assets and `--no-javascript` disables
page scripting. HttpServer is a minimal blocking single-threaded HTTP/1.1
adapter; WebInterface can also route WebRequest values directly in an embedder.

```sh
curl -s http://127.0.0.1:8080/api/health
curl -s -X POST http://127.0.0.1:8080/api/sessions \
  -H 'Content-Type: application/json' -d '{}'
```

Use the returned session ID for subsequent actions:

```sh
curl -s -X POST http://127.0.0.1:8080/api/sessions/SESSION_ID/scrape \
  -H 'Content-Type: application/json' \
  -d '{"html":"<h1>Offline</h1>","selectors":["h1"]}'
```

| Method/path | Purpose / body |
|---|---|
| `GET /api/health`, `/api/capabilities` | Version/session count and capability levels |
| `GET/POST /api/sessions` | List/create sessions |
| `GET/DELETE /api/sessions/{id}` | Summary/close |
| `POST .../navigate` | Navigate with `{ "url": "..." }` |
| `POST .../content`, `.../text`, `.../links` | Inspect document |
| `POST .../scrape` | CSS extraction with `selectors` array |
| `POST .../xpath` | Evaluate `expression` |
| `POST .../evaluate` | Evaluate `script`, optional navigation URL |
| `POST .../endpoints` | Heuristic discovery and OpenAPI output |

Document inspection actions accept the current page or supplied html/url input.
Errors use JSON `error`/`message` and HTTP status. Endpoint actions honor the
supported extraction options and preserve observed-network provenance.
Raw HTML/text/evaluation and automation credential endpoints are host-control
data; they are not equivalent to PDQL's sanitized snapshot export. The listener
has no built-in authentication service, so the default loopback address is the
normal local-client deployment.

## WebDriver

```sh
prowsetk webdriver --host 127.0.0.1 --port 9515
```

Connect a Selenium client with a prowsetk browser capability:

```python
from selenium import webdriver
from selenium.webdriver.common.options import ArgOptions

options = ArgOptions()
options.set_capability('browserName', 'prowsetk')
driver = webdriver.Remote('http://127.0.0.1:9515', options=options)
try:
    driver.get('https://example.com/')
    print(driver.title)
finally:
    driver.quit()
```

The implemented protocol covers session lifecycle, navigation, document/source,
CSS/ID/tag/name/XPath lookup, synthetic element actions, JS evaluation, cookies,
timeouts, window handles, and stale-element errors. Unsupported actions retain
the engine's restrictions; there is no browser-rendered screenshot/layout.

## CDP and Playwright

```sh
prowsetk cdp --host 127.0.0.1 --port 9222
```

`playwright` is an alias for the same native CDP bridge:

```python
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    browser = p.chromium.connect_over_cdp('http://127.0.0.1:9222')
    page = browser.contexts[0].new_page()
    page.goto('https://example.com/')
    print(page.title())
    browser.close()
```

Discovery endpoints are `/json/version` and `/json/list`; target/session,
runtime evaluation, DOM, and navigation commands map to Flatworm. Screenshot
responses are protocol-valid placeholders. Compatibility is this documented
CDP subset rather than every Chromium behavior. Use explicit ports; port 0
prints an available listener address.

## Service-layer web gateway

`interface/web` is a FastAPI/Uvicorn/HTTPX application delegating browser work
to `prowsetk serve`. Its dashboard and `/api/v1` API add queued endpoint runs,
poll/cancel/result operations, and artifact downloads. From that directory,
`./scripts/install.sh` and `./scripts/dev.sh` prepare/start the gateway;
`./scripts/run-stack.sh` starts it with the upstream.

Key routes are `/api/v1/health`, `/sessions`, `/sessions/{id}/navigate`,
`/runs`, `/runs/{id}`, `/runs/{id}/cancel`, `/runs/{id}/result`, and
`/runs/{id}/openapi.yaml`. POST runs with a URL and optional session_id and
discover_options; keep polling until a terminal run status.

Environment names use `PROWSETK_WEB_`: HOST (127.0.0.1), PORT (8090),
PROWSETK_UPSTREAM (http://127.0.0.1:8080), DEFAULT_TIMEOUT_SECONDS (25.0),
POLL_INTERVAL_SECONDS (1.2), and MAX_RUNS (300). Run state is in-memory;
queued work/artifacts do not become a durable crawler cache. The gateway
enforces provenance and redaction on discovery options. See its API.yaml for
request/result models.

## pyprowsetk

The Python package binds C++ through nanobind and adds convenience helpers.
Each build preset keeps its module, wrapper, and generated stubs in the binary
directory:

```sh
PYTHONPATH=build/default/interface/pyprowsetk python3 -c \
  'import pyprowsetk as pk; print(pk.version())'
```

```python
import pyprowsetk as pk

browser = pk.create_browser(javascript=False)
with browser.create_session() as session:
    session.load_html('<title>Local</title><h1>Hello</h1>', 'https://example.test/')
    print(session.document().query_selector('h1').text())

document = pk.open_html('<table><tr><th>Name</th></tr><tr><td>Alice</td></tr></table>')
assert pk.extract_tables(document) == [[{'Name': 'Alice'}]]
```

create_browser defaults to JavaScript false and a Python-specific user agent.
BrowserConfig itself retains core defaults. Helpers include document_to_dict,
element_to_dict, table extraction, crawl, python_hooks, and optional pandas
conversion. Element/KeyValueStore provide dict-like access. Direct dict helpers
contain raw page data; select/redact before publishing private pages.

The Python crawl helper traverses links with its own simplified host/depth
selection. It does not provide crawler's transport quotas/robots contract.
Core capabilities remain authoritative for bound APIs; Python does not add
rendering or enable the disabled WASM backend. The current direct bindings
cover compatibility IRs rather than the newer canonical ProwseEvent/PDQL
entrypoints; use their C/C++/Lua APIs for those operations. CTest handles Python
sanitizer runtime preloading and preset isolation.

Reference: `web_interface.hpp`, [gateway notes](../interface/web/README.md),
[Python notes](../interface/pyprowsetk/README.md).

**Next:** [Prowse-TUI](29-prowse-tui.md).
