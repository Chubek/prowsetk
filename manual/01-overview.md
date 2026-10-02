# Chapter 01: Overview

[Manual index](README.md)

## What ProwseTk provides

ProwseTk is an embeddable C++20 toolkit for programmable, headless web browsing.
Its Flatworm engine loads HTML, maintains a queryable DOM, executes page
JavaScript through QuickJS, and mediates HTTP requests through browser sessions.
It supports scraping, endpoint discovery, API automation, testing, and data
extraction without a display server or a graphical browser engine.

A useful workflow is:

```text
Browser → Session → HTML/JavaScript → Document → selection → extracted data
                              ↓                         ↓
                         HTTP events              page IR / PDQL
```

The core builds independently of optional interfaces. Features such as HTTPS,
XPath, Python bindings, persistent spider caches, and eBPF depend on the
available libraries and build configuration.

## Execution layers

| Layer | Responsibility | Typical interface |
|---|---|---|
| C++ | Host application, engine, native plugins | `Browser`, `Session`, plugin registry |
| Lua | Driver orchestration and extensions | `lprowse`, `lprowsext`, tool modules |
| JavaScript | Scripts belonging to the loaded page | QuickJS web-platform shim |
| WASM | Portable plugin contract behind a runtime abstraction | `WasmRuntime`, WIT |

Lua objects are managed host handles. Page JavaScript receives opaque element
handles through the web-platform shim. Native plugins cross a versioned C ABI.
WASM contracts are defined in WIT; the current runtime implementation reports
disabled support, including with the WASM build preset.

## Terms used throughout the manual

- **Browser:** owns configuration, transport, storage, events, and plugins.
- **Session:** a browsing context with a current URL, headers, document, and
  page-script runtime.
- **Document / Element:** inspectable and mutable DOM objects.
- **Driver:** a Lua script with an entrypoint, conventionally `main(args)`.
- **Extension:** Lua code adding extraction or automation behavior to a host.
- **Plugin:** a native, Lua, or WASM registration; its implementation and loader
  determine which hooks actually run.
- **IR:** an intermediate page representation. The canonical IR is the
  ProwseEvent start/attribute/text/end stream.
- **Projection:** selected PDQL fields serialized as result rows.

## Compatibility model

Flatworm implements an automation-focused web-platform subset. It has no layout
tree, rasterizer, GPU rendering, or coordinate hit testing. CSS selectors and
XPath select DOM structure; inline styles and attributes supply visibility
heuristics for interaction. PDF generation is a downstream print-layout tool.

Runtime capabilities use five classifications: fully implemented, partially
implemented, implemented with restrictions, dummy implementation, and
unsupported. Consult them when a workflow depends on a particular API. A
browser-like user agent identifies a session but does not change its support
level.

The common limitations are documented in the relevant chapters: tolerant HTML
parsing, a restricted selector grammar, bounded PDQL, partial page JavaScript,
polling watchers, heuristic endpoint/schema discovery, and process-local session
credentials in daemon tools.

## Choosing an entrypoint

Use C++ for embedding and transport policy, Lua drivers for programmable
workflows, `crawler` for a bounded one-shot crawl, `pgwatchd` for selected-data
monitoring, and spider for durable crawl scheduling and caches. Use Beacon when
a user-approved Firefox tab supplies live browser data. The web and Python
interfaces expose the same engine to other clients.

Reference: repository `README.md`; public headers under `include/prowsetk/`.

**Next:** [Build and installation](02-build-and-installation.md).
