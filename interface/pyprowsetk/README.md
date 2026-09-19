# pyprowsetk

Python interface for [ProwseTk](https://github.com/prowsetk/prowsetk) — headless, programmable browser toolkit.

Uses [nanobind](https://github.com/wjakob/nanobind) to bind C++ to Python. All ProwseTk features are accessible, plus Python-exclusive conveniences.

## Features

- **Full coverage**: `Browser`, `Session`, `Document`, `Element`, `NetworkClient`, `Storage`, `JavaScriptRuntime`, `LuaRuntime`, `WebPlatform`, `CapabilitySet`, `EventDispatcher`, `EndpointExtractor`, IR (`ProwseXAS`/`ProwseDOM`/`ProwseVTD`/`ProwseIML`), `PluginRegistry`, `WasmRuntime`, `ProjectConfig` (`Prowse.toml`), `WebInterface`, `Redaction`, URL utilities, XPath.

- **Python-exclusive**:
  - Context managers: `with Browser() as b:` / `with b.create_session() as s:`
  - `create_browser()` factory with Pythonic defaults
  - `open_html(html)` direct parsing without a session
  - `document_to_dict` / `element_to_dict` serialization
  - `extract_table` / `extract_tables` HTML table → `list[dict]`
  - `crawl(start_url, max_depth, same_origin)` pure-Python crawler (alternative to Lua `crawl-site` driver)
  - `python_hooks(browser)` decorator registry for events (`@hooks.before_request`)
  - `to_dataframe()` optional pandas export
  - `Element` dict-like access: `el["href"]`, `el["href"] = "..."`, `"href" in el`
  - `KeyValueStore` dict-like access: `store["key"]`
  - Iteration helpers and `__repr__` / `__bool__` throughout

## Install (development)

```sh
cmake --preset default
cmake --build --preset default   # also copies _core extension into pyprowsetk/
PYTHONPATH=interface/pyprowsetk:$PYTHONPATH python -c "import pyprowsetk; print(pyprowsetk.version())"
```

Or via pip (scikit-build-core):

```sh
pip install -e interface/pyprowsetk
```

## Quick start

```python
import pyprowsetk as pk

# Simple navigation with memory client (offline / hermetic)
browser = pk.create_browser()
client = pk.MemoryNetworkClient()
client.set_response("https://example.com", pk.HttpResponse())
# ... configure response body
browser.set_network_client(client)

with browser.create_session() as sess:
    sess.load_html('<html><title>Hello</title><a href="/next">next</a></html>', "https://example.com")
    doc = sess.document()
    print(doc.title())  # Hello
    print(doc.query_selector("a").attribute("href"))

# Direct HTML parsing (Python-exclusive)
doc = pk.open_html('<table><tr><th>Name</th><th>Age</th></tr><tr><td>Alice</td><td>30</td></tr></table>')
print(pk.extract_tables(doc))  # [{'Name': 'Alice', 'Age': '30'}]

# Python hooks (Python-exclusive)
hooks = pk.python_hooks(browser)
@hooks.before_request
def log_req(ev):
    print("request", ev.url)

# Endpoint extraction (full engine feature)
extractor = pk.EndpointExtractor()
result = extractor.extract(doc)
print(result.openapi_yaml)
```

## Architecture

Four execution layers are preserved (C++ / Lua / WASM / JS). The binding never leaks raw Wasmtime handles or C++ types across the plugin ABI boundary; `WasmRuntime` stays behind its abstraction.

## License

MIT
