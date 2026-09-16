# Lua Modules

ProwseTk exposes two Lua modules from `LuaRuntime`:

- `lprowse` drives managed `Browser`, `Session`, `Document`, and `Element`
  userdata.
- `lprowsext` provides XPath helpers, endpoint extraction, document
  processors, extractors, and managed WASM module handles.

The `init.lua` files are standalone surface mirrors and API documentation. In
an embedded ProwseTk runtime, `require("lprowse")` and
`require("lprowsext")` resolve to the native modules registered by
`LuaRuntime`; the mirrors deliberately fail clearly outside that runtime.

## Control API

`Session` supports navigation, offline HTML loading, requests, JavaScript
evaluation, scoped event subscriptions, capabilities, and request headers.
`Document` supports CSS and XPath querying, metadata, links/forms/scripts and
resource discovery, and detached-element creation. `Element` supports DOM
inspection, traversal, selector queries, form values, and mutation.

All browser-facing values are managed Lua userdata. Lua receives neither raw
C++ objects nor Wasmtime handles. The `lprowsext.wasm` API reports a disabled
runtime when ProwseTk is built without a WASM backend; WASI remains disabled by
default.

## Endpoint Extraction

`lprowsext.endpoints.extract(document_or_session, options)` returns a managed
result. Its `endpoints()` values include `source`, `discovery_method`, and
`confidence`, because discovery is heuristic. `openapi_yaml()` and
`write_openapi_yaml(path)` retain ProwseTk's default redaction behavior.
