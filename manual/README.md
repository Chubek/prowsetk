# ProwseTk Manual

This manual covers the current ProwseTk implementation: the Flatworm headless
engine, C/C++ and Lua interfaces, plugins, extensions, command-line tools, and
client interfaces. It is a practical guide to building and operating the
software, with reference tables and examples for extending it.

Each chapter is a separate Markdown file. Read them in order for a complete
introduction, or use the subject index below. The repository `README.md` remains
the architecture of record; public headers and the referenced implementation
files define the callable interfaces.

## Chapters

### Getting started

1. [Overview](01-overview.md) — architecture, terminology, and compatibility.
2. [Build and installation](02-build-and-installation.md) — dependencies,
   presets, installation, tests, and documentation builds.
3. [Quick start and CLI](03-quick-start-and-cli.md) — offline workflows and the
   implemented `prowsetk` commands.
4. [Project configuration](04-project-configuration.md) — `Prowse.toml`, driver
   arguments, paths, and configuration consumption.

### Engine and programming interfaces

5. [C++ embedding](05-cpp-embedding.md) — ownership, sessions, errors, and CMake.
6. [Sessions and networking](06-sessions-and-networking.md) — requests,
   redirects, HTTPS, proxies, and replaceable transports.
7. [DOM, selectors, and XPath](07-dom-selectors-and-xpath.md) — parsing,
   inspection, mutation, selector syntax, and XPath results.
8. [PDQL](08-pdql.md) — supported query syntax, projections, serializers, and
   the C/C++ and Lua query APIs.
9. [Lua control API](09-lua-control-api.md) — native `lprowse` objects and methods.
10. [Drivers](10-drivers.md) — complete driver recipe, shipped drivers, and
    Booking.com workflows.
11. [Lua extensions](11-lua-extensions.md) — `lprowsext`, processors, extractors,
    module loading, and host-bound execution.
12. [JavaScript](12-javascript.md) — page scripting, web-platform bindings,
    synthetic interactions, and lifecycle work.
13. [Events and diagnostics](13-events-and-diagnostics.md) — callbacks, errors,
    capabilities, redaction, limits, and troubleshooting.
14. [Cookies and storage](14-cookies-and-storage.md) — cookie scope, import,
    storage backends, and persistence contracts.

### Discovery, authentication, and plugin development

15. [Endpoint discovery](15-endpoint-discovery.md) — extractor options,
    observations, provenance, and OpenAPI output.
16. [scrape-endpoints](16-scrape-endpoints.md) — discovery, filtering, recursive
    resolution, Postman export, and assistant-browser metadata.
17. [ezlogin](17-ezlogin.md) — session credentials, form login, and OAuth helpers.
18. [Anti-bot and captcha-handler](18-anti-bot-and-captcha-handler.md) — detection
    signals, handling plans, and confirmed-session workflows.
19. [restful-resolver](19-restful-resolver.md) — bounded iterative GET resolution.
20. [schema-grabber](20-schema-grabber.md) — inferred request/response schemas
    and typed URL parameters.
21. [Native and WASM plugins](21-native-and-wasm-plugins.md) — registry lifecycle,
    C ABI, WIT contracts, and runtime availability.

### Intermediate representations and tools

22. [Intermediate representations](22-intermediate-representations.md) —
    ProwseEvent, NDJSON, VTD, IML, walkers, and custom emitters.
23. [Page conversion tools](23-page-conversion-tools.md) — `page2pdf`,
    `page2latex`, templates, and pipelines.
24. [Crawler](24-crawler.md) — Lua/TOML crawling, login, assistants, and exports.
25. [Pagewatch](25-pagewatch.md) — deployment, PDQL watchers, IPC, actions, and
    restart recovery.
26. [Spider](26-spider.md) — persistent crawl frontiers, LMDB cache, controllers,
    and `lspider` drivers.
27. [Beacon](27-beacon.md) — Firefox addon installation, Native Messaging,
    Flashes, consent, and tracepoints.
28. [Web and Python interfaces](28-web-and-python-interfaces.md) — REST,
    WebDriver, CDP/Playwright, the service gateway, and `pyprowsetk`.
29. [Prowse-TUI](29-prowse-tui.md) — terminal navigation, exports, help,
    configuration, and extensions.
30. [eBPF interface](30-ebpf-interface.md) — optional libbpf runtime and
    host-mediated endpoint observations.

## Conventions

- Shell examples assume the repository root as the working directory, unless a
  chapter says otherwise. Installed executables are assumed to be on `PATH`;
  Chapter 2 shows how to use build-tree binaries.
- `example.test` URLs in offline examples are base URLs, not public services.
  Commands that navigate `https://example.com/` perform real network requests.
- Lua examples run inside ProwseTk's `LuaRuntime`. The native modules are
  supplied by that host. Plugin Lua files can be loaded explicitly with
  `dofile` from the repository or installed data directory.
- Defaults in tables refer to the named API or tool. Core browser defaults,
  plugin defaults, and daemon defaults can differ.
- A capability classification describes the implementation's support level.
  Endpoint discovery, schema inference, and anti-bot detection retain their
  provenance and confidence; completeness metadata has the specific meaning
  described in its chapter.

## Coverage and source map

| Area | Chapters | Primary source locations |
|---|---|---|
| Core API and Flatworm | 5–8, 12–15 | `include/prowsetk/`, `src/core/`, `src/flatworm/`, `src/pdql/` |
| CLI and project settings | 3–4, 10 | `src/cli/prowsetk_main.cpp`, `src/core/project_config.cpp`, `drivers/` |
| Native Lua modules and extensions | 8–11, 22 | `src/core/lua_runtime.cpp`, `lua/` |
| Authentication/discovery plugins | 16–20 | `plugins/scrape-endpoints`, `ezlogin`, `captcha-handler`, `restful-resolver`, `schema-grabber` |
| Plugin ABI and WASM design | 21 | `ProwseTk-Plugin.h`, `plugin_registry.hpp`, `wasm_runtime.hpp`, `wit/` |
| IR consumers | 22–23 | `src/core/ir_*.cpp`, `tools/page2pdf`, `tools/page2latex` |
| Crawling and watching | 24–26 | `tools/crawler`, `tools/pagewatch`, `plugins/spider`, `tools/automation` |
| Browser handoff | 16, 18, 24, 27 | assistant-browser settings, `plugins/beacon`, Booking.com examples |
| Service and client interfaces | 28 | `web_interface.hpp`, `interface/web`, `interface/pyprowsetk` |
| Terminal and instrumentation | 29–30 | `tools/prowse-tui`, `plugins/ebpf-interface` |

The historical plugins under `plugins/.deprecated/` are covered as migration
context in Chapter 16. They are outside the current plugin workflow.

## Building the manual

With Pandoc installed, run:

```sh
bash scripts/build-docs.sh
```

The output is `build/docs/html/index.html` and
`build/docs/latex/prowsetk.tex`. Supply a directory argument to choose another
output root. The builder checks for this index and exactly 30 numbered chapters.
Chapter navigation becomes internal links in the combined outputs; links to
repository references remain relative to each generated file's location.
