# ProwseTk

ProwseTk is an embeddable C++ toolkit for building programmable, headless web
browsers. It targets web scraping, endpoint discovery, API automation, testing,
and data-extraction workflows that need browser-like document and script
behavior without a graphical user interface.

ProwseTk does not depend on WebKit, Blink, or Gecko. It ships its own lightweight
engine, **Flatworm**, optimized for automation and programmability rather than
complete browser compatibility or pixel-perfect rendering.

## Table of Contents

- [Design Goals](#design-goals)
- [Architecture](#architecture)
- [Flatworm](#flatworm)
- [Parsing HTML and CSS](#parsing-html-and-css)
- [Public C++ API](#public-c-api)
- [Lua Control Layer (`lprowse`)](#lua-control-layer-lprowse)
- [Lua Extensions (`lprowsext`)](#lua-extensions-lprowsext)
- [Plugin System](#plugin-system)
- [WASM Runtime](#wasm-runtime)
- [Endpoint Extraction](#endpoint-extraction)
- [JavaScript Execution](#javascript-execution)
- [Asynchronous Operation](#asynchronous-operation)
- [Events and Hooks](#events-and-hooks)
- [Unsupported Web APIs](#unsupported-web-apis)
- [Diagnostics and Instrumentation](#diagnostics-and-instrumentation)
- [Security and Resource Limits](#security-and-resource-limits)
- [Project Configuration (`Prowse.toml`)](#project-configuration-prowsetoml)
- [Build and Runtime Strategy](#build-and-runtime-strategy)
- [Dependencies](#dependencies)
- [Compatibility Policy](#compatibility-policy)
- [Examples](#examples)
- [Non-Goals](#non-goals)
- [Repository Layout](#repository-layout)

## Design Goals

1. A small, embeddable C++ core.
2. Reliable headless navigation.
3. Lua-driven browser automation.
4. JavaScript execution through QuickJS.
5. A stable C/C++ plugin interface (`ProwseTk-Plugin.h`).
6. Lua-based extensibility.
7. Isolated browser sessions.
8. Deterministic automation behavior.
9. Detailed diagnostics and instrumentation.
10. Practical support for scraping and endpoint automation.
11. Minimal graphical and system dependencies.
12. Explicit documentation of browser compatibility.

## Architecture

ProwseTk uses a layered extension model. Each layer has a distinct
responsibility, and the WebAssembly runtime is an implementation detail hidden
behind an internal interface.

```text
ProwseTk core
    |
    +-- Native C/C++ plugins through ProwseTk-Plugin.h
    |
    +-- Lua drivers and extensions through lprowse / lprowsext
    |
    +-- WASM modules/components through a capability-limited host
```

Four execution environments participate in a session:

- **C++** hosts ProwseTk and implements the core engine.
- **Lua** drives browser sessions and provides lightweight scripting extensions.
- **WASM** provides portable, sandboxed plugins for heavier or untrusted
  extensions.
- **JavaScript** remains the page scripting runtime inside Flatworm.

### Extension layers at a glance

```text
                         ProwseTk Host
                              |
                    +---------+---------+
                    |                   |
              Native plugins       Flatworm
          ProwseTk-Plugin.h        browser engine
                    |                   |
                    +---------+---------+
                              |
                  WASM runtime abstraction
                         Wasmtime
                              |
                    WASM plugin components
                              |
              +---------------+---------------+
              |                               |
        ProwseTk-WASM SDK                 WASI capabilities
        guest-side APIs                   filesystem, clocks,
                                          sockets, randomness
```

Native plugins can load WASM components, register them with the host, and expose
them through ProwseTk. Lua scripts never receive unrestricted access to the
underlying Wasmtime objects; they interact with managed handles only.

## Flatworm

Flatworm is ProwseTk's headless browser engine. It provides the core
functionality required to load, inspect, and interact with web documents.

Flatworm is not a complete replacement for a general-purpose browser engine. It
focuses on behavior useful to automation applications:

- HTTP and HTTPS navigation
- Redirect handling
- URL resolution and normalization
- HTML parsing
- DOM construction and traversal
- Element selection and manipulation
- Form inspection and submission
- Cookies and request headers
- JavaScript execution
- Basic browser storage
- Network request interception
- Resource discovery
- Event-driven navigation
- Script and network diagnostics
- Plugin and extension integration

Flatworm implements standards-based behavior where required by supported
workflows. Features that are not useful for headless automation may be omitted,
simplified, or represented by configurable dummy implementations.

Flatworm does not require:

- A display server
- A windowing system
- GPU acceleration
- A graphical desktop environment
- A complete CSS layout engine
- Pixel rendering
- A full browser user interface

## Parsing HTML and CSS

ProwseTk does not render pages, but it retains the capability to do so. It parses
page HTML into a DOM (Document Object Model), which enables JavaScript evaluation
and a broad range of document-processing capabilities. The DOM is provided by
NetSurf's DOM library (`libdom`). HTML parsing uses `lexbor`, with
`gumbo-parser` available as a lenient fallback.

The current core also ships **Flatworm's built-in tolerant parser**, a
dependency-free implementation of the `Document`/`Element`/CSS-selector slice.
It exists so the engine configures, builds, and tests with no third-party HTML
toolchain present, and it defines the DOM contract that the `lexbor` and
`gumbo-parser` backends will satisfy. Selector support covers type, class, id,
attribute operators, the descendant/child/adjacent/general-sibling combinators,
selector lists, and the structural `:first-child`, `:last-child`,
`:only-child`, `:nth-child()`, `:empty`, `:root`, and `:not()` pseudo-classes.

An XPath interface into the DOM is exposed through the Lua extension layer as
`lprowsext.dom.xpath`. XPath substantially increases scraping reach compared with
plain CSS selectors.

## Public C++ API

The public C++ API separates browser responsibilities into focused components.
All components are replaceable or extendable through documented interfaces.

### `Browser`

Owns global configuration and creates browser sessions.

- Engine configuration
- Network configuration
- Plugin loading
- Lua runtime configuration
- Logging configuration
- Global resource limits
- Session creation and destruction

### `Session`

Represents an isolated browsing context.

A session may contain its own:

- Current URL
- Cookies
- Local storage
- Session storage
- JavaScript state
- Request headers
- Proxy settings
- Navigation history
- Plugin state
- Security policy
- Timeout configuration

Multiple sessions run independently within the same browser instance.

### `Document`

Represents the currently loaded HTML document.

- Title access
- URL and base URL access
- Text extraction
- CSS selector queries
- Element lookup
- Metadata inspection
- Link and form discovery
- Script and resource discovery
- DOM traversal

### `Element`

Represents a DOM element or node.

- Tag-name access
- Attribute access
- Text and HTML extraction
- Child and parent traversal
- CSS selector queries
- Form values
- DOM mutation
- Visibility or availability metadata where applicable

### `NetworkClient`

Performs HTTP and HTTPS requests.

- Request construction
- Response processing
- Redirect handling
- Cookie integration
- Header management
- Connection reuse
- Timeouts
- Proxy support
- TLS configuration
- Request interception
- Response interception
- Download limits
- Compression handling

### `JavaScriptRuntime`

Provides JavaScript execution through QuickJS.

- Script evaluation
- Execution limits
- Memory limits
- Exception reporting
- Console event forwarding
- Host-object bindings
- Promise and timer integration
- Page-context isolation

### `LuaRuntime`

Provides Lua execution for automation and extensions.

- Loading Lua scripts
- Calling Lua functions from C++
- Exposing browser objects to Lua
- Registering Lua event handlers
- Loading Lua extensions
- Runtime limits
- Error propagation
- Per-session or shared Lua state

### `WebPlatform`

Provides browser-compatible Web API bindings. Every API carries an
implementation classification (see [Compatibility Policy](#compatibility-policy));
the classification is available in documentation and through a runtime
capability query.

### `Storage`

Provides storage backends for:

- Cookies
- Local storage
- Session storage
- Cache data
- Application-defined persistence

Storage is replaceable through C++ interfaces.

### `EventDispatcher`

Delivers events related to:

- Navigation
- Requests
- Responses
- Redirects
- Document loading
- DOM changes
- JavaScript execution
- JavaScript exceptions
- Console output
- Unsupported APIs
- Plugin lifecycle
- Session lifecycle

## Lua Control Layer (`lprowse`)

ProwseTk uses Lua as its primary scripting and automation language.
`lprowse` is the primary programmable browser API. Applications use it to
control browser sessions, navigate pages, inspect documents, execute JavaScript,
process results, and coordinate multiple sessions.

```lua
local prowse = require("lprowse")

local browser = prowse.browser.new()
local session = browser:create_session()

session:navigate("https://example.com")

local document = session:document()

print(document:title())

for _, link in ipairs(document:query_selector_all("a")) do
    print(link:attribute("href"))
end
```

`lprowse` exposes:

- Browser creation
- Session management
- Navigation
- HTTP requests
- Document access
- CSS selector queries
- Element inspection
- Form interaction
- Cookie management
- Storage access
- JavaScript evaluation
- Event subscriptions
- Request and response hooks
- Logging and diagnostics
- Capability queries
- Plugin management

Lua scripts may be loaded from files, passed as strings, or embedded directly
into C++ applications.

## Lua Extensions (`lprowsext`)

`lprowsext` and its submodules extend ProwseTk itself. Lua extensions may:

- Register new browser commands
- Add document extraction helpers
- Define reusable navigation workflows
- Subscribe to browser events
- Modify or reject requests
- Inspect and transform responses
- Add custom output formats
- Register endpoint detectors
- Provide domain-specific automation APIs
- Coordinate multiple browser sessions
- Define custom diagnostics
- Compose existing C++ plugins

```lua
local prowse = require("lprowse")
local ext = require("lprowsext")

local extractor = ext.extractor.new()

extractor:on_document(function(document)
    return {
        title = document:title(),
        links = document:query_selector_all("a")
    }
end)

local browser = prowse.browser.new()
browser:install_extension(extractor)

local session = browser:create_session()
session:navigate("https://example.com")
```

Lua extensions expose both synchronous and asynchronous behavior. They may use
the engine's event loop, timers, request hooks, and session lifecycle events.

## Plugin System

ProwseTk provides a native plugin interface through `ProwseTk-Plugin.h`. The
interface allows native components to extend the browser without modifying the
Flatworm core.

Plugins may provide:

- Request and response handlers
- Custom resource loaders
- DOM processors
- Document extractors
- JavaScript bindings
- Lua modules
- Lua extension functions
- Storage backends
- Authentication handlers
- Cookie policies
- Event listeners
- Output generators
- Capability providers
- Diagnostics and instrumentation
- Custom automation commands

### Native plugin ABI

`ProwseTk-Plugin.h` is the native plugin entry point. It uses a C-compatible ABI
so plugins remain binary-compatible across supported C++ compilers and standard
libraries.

```c
#ifndef PROWSETK_PLUGIN_H
#define PROWSETK_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROWSETK_PLUGIN_ABI_VERSION 1

/* Exported entry-point symbol: a shared object defines
 *   const ProwseTkPlugin* prowsetk_plugin_entry(void);
 */
#define PROWSETK_PLUGIN_ENTRY_SYMBOL "prowsetk_plugin_entry"

#if defined(_WIN32)
#define PROWSETK_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PROWSETK_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef struct ProwseTkHost ProwseTkHost;

typedef enum {
    PROWSETK_PLUGIN_NATIVE = 1,
    PROWSETK_PLUGIN_LUA = 2,
    PROWSETK_PLUGIN_WASM = 3
} ProwseTkPluginType;

typedef enum {
    PROWSETK_LOG_TRACE = 0,
    PROWSETK_LOG_DEBUG = 1,
    PROWSETK_LOG_INFO = 2,
    PROWSETK_LOG_WARN = 3,
    PROWSETK_LOG_ERROR = 4
} ProwseTkLogLevel;

/* Host services made available to plugins. The struct is owned by the host and
 * remains valid for the plugin's lifetime. */
typedef struct {
    uint32_t abi_version;
    void* user_data;
    void (*log)(void* user_data, int level, const char* message);
} ProwseTkHostApi;

struct ProwseTkHost {
    const ProwseTkHostApi* api;
};

typedef struct {
    const char* name;
    const char* version;
    const char* abi_version;
    const char* description;
    ProwseTkPluginType type;
    const char* const* capabilities;
    size_t capability_count;
} ProwseTkPluginInfo;

/* Return 0 on success, nonzero on failure. Implementations must not let C++
 * exceptions escape across this boundary. */
typedef struct {
    int (*initialize)(ProwseTkHost* host);
    void (*shutdown)(ProwseTkHost* host);
    const ProwseTkPluginInfo* (*info)(void);
} ProwseTkPlugin;

typedef const ProwseTkPlugin* (*ProwseTkPluginEntry)(void);

#ifdef __cplusplus
}
#endif

#endif /* PROWSETK_PLUGIN_H */
```

A plugin exposes a well-defined entry point plus metadata describing its name,
version, ABI compatibility, dependencies, and capabilities. The full interface
must specify:

- ABI versioning
- Plugin discovery
- Plugin loading and unloading
- Initialization and shutdown
- Capability registration
- Event subscription
- Error reporting
- Configuration
- Threading rules
- Memory ownership
- Lua module registration
- C++ exception boundaries
- Compatibility guarantees

The C++ registry mirrors this functionality and may evolve independently of the C
ABI:

```cpp
namespace prowsetk {

class PluginRegistry {
public:
    void load_native(const std::filesystem::path& path);
    void load_lua(const std::filesystem::path& path);
    void load_wasm(const std::filesystem::path& path,
                   const WasmPluginConfig& config);
};

}
```

### Plugin configuration

Plugin sandbox and capability settings are namespaced per plugin, because a
single shared `[plugins.sandbox]` table cannot express per-plugin settings
safely for an array of tables. The canonical form is `[plugin_config.<name>]`:

```toml
[[plugins]]
name = "endpoint-extraction"
description = "Discover web endpoints and generate OpenAPI specifications"
type = "wasm"
path = "plugins/endpoint_extraction.wasm"
enabled = true
autoload = true

[plugin_config.endpoint-extraction]
component = true
wasi = false
max_memory_mb = 128
max_table_elements = 10000
execution_timeout_ms = 30000
fuel = 10000000

[plugin_config.content-classifier]
component = true
wasi = false
max_memory_mb = 256
execution_timeout_ms = 10000
```

### WASM plugins and the Component Model

For new WASM plugins, prefer the **WebAssembly Component Model** with
WIT-defined interfaces. Do not build the plugin API on raw C ABI functions such
as:

```cpp
plugin->on_request(char* url, char* headers);
```

That approach creates difficult problems around memory ownership, string
encoding, ABI compatibility, struct layout, version negotiation, error
propagation, host/guest allocation, and C++ compiler differences. Interfaces are
instead defined in WIT:

```wit
package prowsetk:plugin@0.1.0;

interface types {
    record request {
        method: string,
        url: string,
        headers: list<tuple<string, string>>,
        body: list<u8>,
    }

    record response {
        status: u16,
        headers: list<tuple<string, string>>,
        body: list<u8>,
    }

    variant hook-result {
        continue,
        reject(string),
        replace-request(request),
    }
}

interface network-hooks {
    before-request: func(req: types.request) -> result<types.hook-result, string>;
    after-response: func(req: types.request, res: types.response)
        -> result<types.hook-result, string>;
}

interface document-extractor {
    extract: func(url: string, html: string) -> result<string, string>;
}

world prowsetk-plugin {
    export network-hooks;
    export document-extractor;
}
```

Split the real interface into small interfaces so a plugin can request only the
capabilities it needs. A component-based plugin may implement:

- Request and response hooks
- Document extraction
- Endpoint detection
- OpenAPI generation
- Response transformation
- Authentication logic
- Content analysis
- Custom Lua-facing operations

### Lua-facing plugin extensions

Native plugins may register Lua modules and extension functions. Lua scripts then
reach those capabilities through `lprowsext` or plugin-specific modules, without
recompiling ProwseTk.

```lua
local ext = require("lprowsext")
local analytics = require("my_analytics_plugin")

ext.register_document_processor("analytics", function(document)
    return analytics:analyze(document)
end)
```

A plugin may provide a native implementation for performance-sensitive work, a
Lua-facing module, a pure Lua extension, or a combination of all three.

## WASM Runtime

### `WasmRuntime` abstraction

The runtime is an implementation detail. ProwseTk depends only on an internal
interface:

```cpp
class WasmRuntime {
public:
    virtual ~WasmRuntime() = default;

    virtual std::shared_ptr<WasmModule>
    load_module(const std::filesystem::path& path) = 0;

    virtual std::shared_ptr<WasmInstance>
    instantiate(const WasmModule& module,
                const WasmSandboxConfig& config) = 0;
};
```

This keeps the rest of ProwseTk independent of Wasmtime and leaves room for
another runtime later. The initial implementation uses **Wasmtime** through its
C API or a small internal C++ wrapper. Wasmtime provides an embeddable runtime,
WASI support, and Component Model support.

### WIT-defined plugin contract

The Component Model provides a stable plugin contract instead of exposing raw
pointers and engine internals to WASM modules. A browser-facing plugin observes
host-normalized events and lets ProwseTk retain responsibility for navigation,
policy enforcement, and network access:

```wit
interface browser-observation {
    on-navigation: func(url: string);
    on-document: func(url: string, html: string);
    on-request: func(request: types.request);
    on-response: func(request: types.request, response: types.response);
    on-script-error: func(url: string, message: string);
}
```

### WASI policy

Do not enable unrestricted WASI by default. A browser plugin generally does not
need arbitrary access to the host filesystem, environment variables, host
processes, system clocks, arbitrary network sockets, or native OS handles.

Default policy:

```toml
[wasm.defaults]
enabled = true
component_model = true
wasi = false
filesystem = "none"
network = "host-mediated"
environment = "none"
max_memory_mb = 128
execution_timeout_ms = 30000
fuel = 10000000
```

Per-plugin capability grants override the defaults:

```toml
[plugin_config.endpoint-extraction]
wasi = true
filesystem = "output-only"
network = "host-mediated"
environment = "none"
clocks = "monotonic"
random = "host-provided"
```

`host-mediated` network access means the plugin requests network operations
through ProwseTk's `NetworkClient`, subject to the browser session's policies.
The WASM plugin must not open arbitrary sockets itself. This is especially
important for endpoint extraction: the extractor observes or requests browser
traffic through ProwseTk instead of independently bypassing domain allowlists,
cookies, proxy settings, TLS policies, request limits, authentication
boundaries, and redaction rules.

### Lua binding for WASM

WASM plugins are exposed to Lua through `lprowsext.wasm`, which offers managed
handles and high-level operations only:

```lua
local prowse = require("lprowse")
local wasm = require("lprowsext.wasm")

local browser = prowse.browser.new()

local plugin = wasm.load("plugins/endpoint_extraction.wasm", {
    memory_limit_mb = 128,
    execution_timeout_ms = 30000
})

browser:install_extension(plugin)

local session = browser:create_session()
session:navigate("https://example.com")

local result = plugin:extract_endpoints(session)
result:write_openapi_yaml("build/openapi.yaml")
```

Recommended Lua-facing operations:

```lua
plugin:name()
plugin:version()
plugin:capabilities()
plugin:call(operation, value)
plugin:close()
```

WASM plugins may export Lua modules, but the host should explicitly register
their functions. This gives ProwseTk control over naming, permissions, error
handling, and lifecycle.

## Endpoint Extraction

ProwseTk includes an **Endpoint Extraction** extension/plugin by default. It
discovers endpoints exposed or referenced by a website and produces an OpenAPI 3.x
YAML specification. It is also the default first WASM plugin because it is mostly
data processing with a clear input/output boundary.

Discovery inspects:

- HTML links
- Forms and form actions
- Script elements
- Inline and external JavaScript
- `fetch` and `XMLHttpRequest` calls
- Static URL strings
- JSON configuration objects
- Embedded API metadata
- Network requests observed during page execution
- Common API path patterns
- HTTP methods
- Query parameters
- Request headers
- Request and response content types
- Example response structures

The host-facing WIT inputs and outputs:

```wit
interface endpoint-extraction {
    record extraction-options {
        follow-links: bool,
        inspect-scripts: bool,
        observe-network: bool,
        infer-schemas: bool,
        max-depth: u32,
        max-pages: u32,
    }

    record extraction-result {
        openapi-yaml: string,
        endpoint-count: u32,
        warnings: list<string>,
    }

    extract: func(
        start-url: string,
        options: extraction-options
    ) -> result<extraction-result, string>;
}
```

Command-line usage:

```sh
prowsetk endpoints \
    --url https://example.com \
    --output openapi.yaml
```

The same functionality is available through Lua:

```lua
local prowse = require("lprowse")
local endpoints = require("lprowsext.endpoints")

local browser = prowse.browser.new()
local session = browser:create_session()

local result = endpoints.extract(session, {
    url = "https://example.com",
    follow_links = true,
    execute_scripts = true,
    observe_network = true
})

result:write_openapi_yaml("openapi.yaml")
```

Because endpoint discovery relies on static analysis, observed network traffic,
and heuristics, generated specifications may be incomplete or uncertain. The
extension preserves provenance and confidence information where possible, for
example through:

- Source URL
- Discovery method
- Observed HTTP method
- Observed status code
- Inferred parameter
- Inferred request body
- Inferred response schema
- Confidence level
- Notes about unsupported or ambiguous behavior

Generated output must:

- Redact cookies and credentials by default
- Mark inferred schemas as inferred
- Preserve discovery provenance
- Record confidence values
- Avoid claiming unsupported certainty
- Emit valid OpenAPI YAML

The extension must never represent an inferred value as definitively known when
the source information is incomplete.

Configurable behavior:

- Crawl depth
- Allowed domains
- URL allowlists and blocklists
- Maximum number of pages
- Maximum number of requests
- JavaScript execution
- Network observation
- Authentication
- Cookie reuse
- Header capture
- Schema inference
- Redaction of secrets
- Output format
- OpenAPI version
- Confidence thresholds

## JavaScript Execution

Web pages may contain JavaScript, so Flatworm uses **QuickJS** as its JavaScript
runtime. QuickJS executes scripts supplied by web pages and scripts injected by
applications. Flatworm exposes a configurable subset of browser APIs to
JavaScript based on their usefulness for supported automation workflows.

The JavaScript environment may provide APIs such as:

- `window`
- `document`
- `location`
- `navigator`
- `console`
- `fetch`
- `XMLHttpRequest`
- `URL`
- `URLSearchParams`
- Timers
- Cookies
- Selected storage APIs
- Form and DOM APIs

The exact API surface is documented by capability level.

Lua does not replace JavaScript as the page scripting language:

- **JavaScript** executes inside the web page context.
- **Lua** drives browser sessions and automation workflows.
- **C++** provides the host application, engine, and native extensions.

This separation lets Lua automation interact with pages while preserving an
isolated JavaScript environment for page scripts.

## Asynchronous Operation

ProwseTk supports both synchronous and asynchronous usage.

Synchronous APIs suit:

- Small scraping tools
- Command-line applications
- Simple document extraction
- One-shot endpoint discovery

Asynchronous APIs suit:

- Multiple concurrent sessions
- Crawling
- Long-running automation
- Network-heavy workflows
- Timers and event handlers
- Streaming extraction
- Interactive Lua applications

Lua APIs may use callbacks, promises, coroutines, or another documented
asynchronous abstraction. The chosen mechanism integrates with the engine's event
loop without requiring a graphical application framework.

## Events and Hooks

Applications and plugins subscribe to browser events and intercept processing
stages.

Important hooks:

- Before navigation
- After navigation
- Before request
- After response
- Before redirect
- After document creation
- Before JavaScript execution
- After JavaScript execution
- JavaScript exception
- Console message
- DOM mutation
- Cookie change
- Storage access
- Unsupported API access
- Plugin initialization
- Plugin shutdown

Handlers may:

- Inspect event data
- Modify selected request properties
- Cancel operations
- Add metadata
- Emit diagnostics
- Schedule follow-up work

Hook behavior is documented with respect to ordering, reentrancy, threading, and
error handling.

## Unsupported Web APIs

Flatworm provides configurable behavior for Web APIs that are not implemented.
For an unavailable API, an application may configure Flatworm to:

- Return an empty or default value
- Expose a dummy object
- Throw a JavaScript exception
- Produce a diagnostic event
- Log a warning
- Terminate the current script
- Reject the operation

Behavior is configurable globally, per browser, per session, or per API category.
The default favors predictable execution and useful diagnostics.

Capabilities are queryable from C++, Lua, and JavaScript where appropriate:

```lua
local capabilities = session:capabilities()

if capabilities:has("fetch") then
    print("Fetch is available")
end
```

## Diagnostics and Instrumentation

ProwseTk provides detailed instrumentation for development and production
environments.

Applications can inspect:

- Navigation timing
- DNS timing
- Connection timing
- TLS timing
- Request and response metadata
- Redirect chains
- JavaScript exceptions
- Console messages
- Unsupported API usage
- DOM processing time
- Lua execution errors
- Plugin failures
- Memory and resource limits
- Endpoint discovery provenance

Logging is configurable by category, severity, session, plugin, output sink, and
structured or plain-text format. The default production configuration avoids
exposing sensitive data such as cookies, credentials, authorization headers, and
private form values.

## Security and Resource Limits

ProwseTk executes untrusted HTML, JavaScript, and potentially Lua extensions, so
applications can configure resource and security limits.

Supported limits:

- Navigation timeout
- Request timeout
- Maximum response size
- Maximum document size
- Maximum script execution time
- Maximum Lua execution time
- JavaScript memory limit
- Lua memory limit
- Maximum redirect count
- Maximum crawl depth
- Maximum number of requests
- Maximum DOM size
- Maximum number of concurrent sessions

Security policies support:

- Domain allowlists
- Domain blocklists
- Scheme restrictions
- Local-file access restrictions
- Private-network access restrictions
- Cross-origin policy configuration
- Script execution control
- Plugin permission declarations
- Request and response redaction

Plugins and Lua extensions declare the capabilities they require.

## Project Configuration (`Prowse.toml`)

A ProwseTk project is configured through `Prowse.toml`. A boilerplate file is
created with `prowsetk init`. The file specifies driver scripts, extensions,
plugins, special commands, and related settings.

```toml
# Prowse.toml
# Project configuration for ProwseTk

[project]
name = "example-api-project"
version = "0.1.0"
description = "Discover and document the endpoints exposed by example.com"
root = "."

[engine]
javascript = true
headless = true
user_agent = "ProwseTk/example-api-project"
follow_redirects = true
max_redirects = 10

# Behavior when a Web API is unavailable. Values may include:
# default, dummy, exception, warn, abort
unsupported_api_behavior = "warn"

observe_network = true

[lua]
version = "5.4"
libraries = ["lprowse", "lprowsext"]
preload = ["lua/bootstrap.lua"]
allow_extensions = true
max_memory_mb = 128
execution_timeout_ms = 30000

[network]
timeout_ms = 30000
connect_timeout_ms = 10000
max_response_size_mb = 32
max_concurrent_requests = 8
dns_cache = true
verify_tls = true
# proxy = "http://127.0.0.1:8080"

[storage]
# Storage is isolated per project unless a session explicitly overrides it.
directory = ".prowse/storage"
cookies = true
local_storage = true
session_storage = true
cache = true
persistent = true

[logging]
level = "info"
format = "text"
file = ".prowse/logs/prowse.log"
categories = [
    "engine",
    "network",
    "javascript",
    "lua",
    "plugin",
    "endpoint_extraction",
    "storage"
]
redact_sensitive_data = true

[output]
directory = "build"
create_directories = true
overwrite = true
formats = ["yaml", "json", "jsonl"]

# ----------------------------------------------------------------------
# Driver scripts
# ----------------------------------------------------------------------

# A driver is a Lua script that controls one or more browser sessions.
# Drivers are invoked with: prowsetk run crawl-site

[[drivers]]
name = "crawl-site"
description = "Crawl the configured site and collect page metadata"
script = "drivers/crawl_site.lua"
entrypoint = "main"
enabled = true

arguments = [
    { name = "url", type = "string", required = true },
    { name = "depth", type = "integer", required = false, default = 2 },
    { name = "output", type = "path", required = false, default = "build/pages.jsonl" }
]

[[drivers]]
name = "login"
description = "Create an authenticated session"
script = "drivers/login.lua"
entrypoint = "main"
enabled = true

arguments = [
    { name = "url", type = "string", required = true },
    { name = "username", type = "string", required = true, secret = true },
    { name = "password", type = "string", required = true, secret = true }
]

# ----------------------------------------------------------------------
# Lua extensions
# ----------------------------------------------------------------------

[[extensions]]
name = "project-helpers"
description = "Project-specific document and request helpers"
type = "lua"
module = "lua/project_helpers.lua"
enabled = true
autoload = true
events = [
    "before_request",
    "after_response",
    "document_loaded",
    "javascript_exception"
]

[[extensions]]
name = "endpoint-tools"
description = "Additional endpoint extraction helpers"
type = "lua"
module = "lua/endpoint_tools.lua"
enabled = true
autoload = true
requires = ["lprowse", "lprowsext"]

# ----------------------------------------------------------------------
# Native and WASM plugins
# ----------------------------------------------------------------------

[[plugins]]
name = "endpoint-extraction"
description = "Discover web endpoints and generate OpenAPI specifications"
type = "native"
path = "plugins/libprowsetk_endpoint_extraction.so"
enabled = true
autoload = true
capabilities = [
    "endpoint-discovery",
    "network-observation",
    "openapi-yaml"
]

[[plugins]]
name = "custom-auth"
description = "Project-specific authentication handlers"
type = "native"
path = "plugins/libprowsetk_custom_auth.so"
enabled = true
autoload = false
capabilities = ["authentication", "request-signing"]
lua_modules = ["prowse_auth"]

# ----------------------------------------------------------------------
# Endpoint extraction
# ----------------------------------------------------------------------

[endpoint_extraction]
enabled = true
plugin = "endpoint-extraction"
base_urls = ["https://example.com"]

follow_links = true
inspect_forms = true
inspect_inline_scripts = true
inspect_external_scripts = true
observe_network = true
inspect_json_config = true

max_depth = 2
max_pages = 100
max_requests = 500
same_origin_only = true

allowed_hosts = ["example.com", "api.example.com"]
blocked_path_patterns = ["/logout", "/delete", "/admin/destructive"]

infer_parameters = true
infer_request_bodies = true
infer_response_schemas = true
infer_status_codes = true
minimum_confidence = 0.50

openapi_version = "3.1.0"
output = "build/openapi.yaml"
include_examples = true
include_provenance = true

redact_cookies = true
redact_authorization_headers = true
redact_query_parameters = [
    "token",
    "access_token",
    "api_key",
    "apikey",
    "password",
    "secret"
]

# ----------------------------------------------------------------------
# Special commands
# ----------------------------------------------------------------------

[[commands]]
name = "endpoints"
description = "Extract endpoints and write an OpenAPI YAML specification"
handler = "endpoint-extraction"
output = "build/openapi.yaml"

arguments = [
    { name = "url", type = "url", required = false },
    { name = "depth", type = "integer", required = false, default = 2 },
    { name = "output", type = "path", required = false, default = "build/openapi.yaml" }
]

[[commands]]
name = "crawl"
description = "Run the main site crawler"
driver = "crawl-site"

arguments = [
    { name = "url", type = "url", required = true },
    { name = "depth", type = "integer", required = false, default = 2 },
    { name = "output", type = "path", required = false, default = "build/pages.jsonl" }
]

[[commands]]
name = "login"
description = "Authenticate and save a session"
driver = "login"

arguments = [
    { name = "url", type = "url", required = true },
    { name = "username", type = "string", required = true, secret = true },
    { name = "password", type = "string", required = true, secret = true }
]

# ----------------------------------------------------------------------
# Browser profiles
# ----------------------------------------------------------------------

[profiles.default]
user_agent = "ProwseTk/example-api-project"
javascript = true
follow_redirects = true
observe_network = true
timeout_ms = 30000

[profiles.endpoint_scan]
user_agent = "ProwseTk/example-api-project endpoint-scanner"
javascript = true
observe_network = true
follow_redirects = true
timeout_ms = 45000
max_response_size_mb = 64

[profiles.static_scan]
user_agent = "ProwseTk/example-api-project static-scanner"
javascript = false
observe_network = false
follow_redirects = true
timeout_ms = 20000

# ----------------------------------------------------------------------
# Security policy
# ----------------------------------------------------------------------

[security]
allowed_schemes = ["https", "http"]
allow_file_urls = false
allow_private_networks = false
allow_loopback = false

# Scripts and plugins must not be able to use arbitrary host resources.
allow_native_lua = true
allow_dynamic_plugin_loading = false

# ----------------------------------------------------------------------
# Session defaults
# ----------------------------------------------------------------------

[sessions]
default_profile = "default"
reuse_cookies = false
persist_session = false
isolate_storage = true

# ----------------------------------------------------------------------
# Project variables
# ----------------------------------------------------------------------

[variables]
site_name = "example"
api_host = "api.example.com"
default_locale = "en-US"
```

## Build and Runtime Strategy

First implementation sequence:

1. Add the `WasmRuntime` abstraction to the C++ core.
2. Implement it with Wasmtime.
3. Start with core WebAssembly modules if Component Model integration is not yet
   required by the minimum toolchain.
4. Define WIT interfaces early.
5. Add Component Model support once the guest and host toolchain is stable.
6. Provide a small `prowsetk-wasm-sdk`.
7. Ship one example plugin and the Endpoint Extraction plugin.
8. Add Lua bindings through `lprowsext.wasm`.
9. Add runtime limits and capability policies before loading third-party
   plugins.

WASM remains optional and is disabled by default until the runtime is a stable
part of the distribution:

```sh
cmake -DPROWSETK_ENABLE_WASM=ON \
      -DPROWSETK_WASM_RUNTIME=wasmtime \
      -DPROWSETK_ENABLE_WASI=OFF
```

A conventional configure/build/test flow uses CMake presets:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

### Extension-mechanism boundaries

WASM does not replace the other extension mechanisms. Use each for its strengths:

- **Native plugins**: low-level engine integration, custom allocators, native
  network transports, storage backends, performance-critical operations, and
  features requiring host OS APIs.
- **Lua**: browser drivers, project-specific automation, configuration, rapid
  prototyping, small extraction rules, and user workflows.
- **WASM**: portable third-party plugins, sandboxed transformations, CPU-heavy
  document processing, untrusted extensions, cross-language plugin development,
  and reusable endpoint and data-analysis components.

## Dependencies

ProwseTk may use the following libraries, either as required dependencies or
optional components depending on the build configuration.

| Library | Role |
|---|---|
| `c-ares` | Asynchronous DNS resolution |
| `fmt` | Type-safe formatting (`{fmt}`) |
| `gumbo-parser` | Lenient HTML parsing fallback |
| `googletest` | CTest-registered unit and integration suites (test-only) |
| `inja` | Template processing for generated output |
| `jemalloc` | Optional memory allocator |
| `kaguya` | C++ and Lua integration |
| `lexbor` | HTML and CSS parsing |
| `libdom` | DOM tree construction (NetSurf) |
| `libev` | Event loop |
| `libmagic` | File and content-type detection |
| `libmill` | Concurrency and coroutine utilities |
| `llhttp` | HTTP/1.1 message parsing |
| `lua` | Automation and extension runtime |
| `mbedtls` | TLS and cryptographic primitives |
| `nexus` | Optional HTTP/3 (QUIC) transport |
| `pugixml` | XML handling and XPath |
| `quickjs` | Page JavaScript runtime |
| `re2` | Safe regular-expression matching |
| `simdjson` | High-performance JSON parsing |
| `spdlog` | Structured and asynchronous logging |
| `tomlplusplus` | `Prowse.toml` parsing |
| `uriparser` | URI parsing and normalization |
| `uvwasi` | WASI system-call support |
| `wasi-libc` | C standard library for WASM |
| `wasi-sdk` | WASI development kit |
| `wasmtime-cpp` | WebAssembly engine |
| `yaml-cpp` | OpenAPI YAML marshalling |
| `zstd` | Compression and decompression |

Build options disable optional dependencies when their functionality is not
required. Dependency versions, licensing, build options, and feature mappings are
documented with the build system.

## Modularity

ProwseTk keeps the following subsystems sufficiently independent to evolve
separately:

- Networking
- DNS resolution
- TLS
- HTTP parsing
- URL processing
- HTML and DOM handling
- JavaScript execution
- Lua execution
- Web API bindings
- Storage
- Event dispatch
- Logging
- Plugin loading
- Endpoint extraction
- Output generation

Applications replace or extend individual components through documented
interfaces. The core toolkit makes no assumptions about graphical output,
display servers, desktop environments, or a specific application framework.

## Compatibility Policy

ProwseTk's compatibility target is practical automation rather than complete
web-platform compliance.

Flatworm documents:

- Implemented Web APIs
- Partially implemented APIs
- Deliberate deviations
- Unsupported browser features
- JavaScript compatibility limitations
- DOM behavior differences
- Networking limitations
- Security-policy differences
- Known incompatibilities with graphical browsers

Every supported feature carries one implementation classification:

- **Fully implemented**
- **Partially implemented**
- **Implemented with restrictions**
- **Dummy implementation**
- **Unsupported**

The same information is available through runtime capability queries where
practical.

## Examples

### C++

```cpp
#include <iostream>
#include <prowsetk/browser.hpp>

int main() {
    prowsetk::Browser browser;

    auto session = browser.create_session();
    session->navigate("https://example.com");

    const auto& document = session->document();

    std::cout << document.title() << '\n';

    for (const auto& link : document.query_selector_all("a")) {
        std::cout << link.attribute("href") << '\n';
    }

    return 0;
}
```

### Lua

```lua
local prowse = require("lprowse")

local browser = prowse.browser.new({
    javascript = true,
    follow_redirects = true,
    timeout = 30
})

local session = browser:create_session()

session:on("console", function(message)
    print("[page]", message.text)
end)

session:navigate("https://example.com")

local document = session:document()
print(document:title())

local links = document:query_selector_all("a")

for _, link in ipairs(links) do
    print(link:text(), link:attribute("href"))
end
```

## Non-Goals

ProwseTk is not intended to:

- Replace full graphical browsers
- Provide pixel-perfect rendering
- Implement every browser Web API
- Guarantee compatibility with all modern websites
- Serve as a general-purpose desktop browser
- Automatically bypass authentication or access controls
- Treat heuristic endpoint discovery as authoritative API documentation

ProwseTk provides a focused, extensible execution environment for applications
that need browser-like navigation, document processing, JavaScript execution,
Lua-driven automation, endpoint extraction, and programmable web interaction
without the cost and complexity of a complete graphical browser engine.

## Repository Layout

```text
prowsetk/
├── CMakeLists.txt          Top-level build definition
├── CMakePresets.json       Configure/build/test presets
├── README.md               This document
├── AGENTS.md               Instructions for implementing agents
├── cmake/                  Build helper modules and dependency wiring
├── include/prowsetk/       Public C++ headers and ProwseTk-Plugin.h
├── src/                    Core engine, Flatworm, and plugin implementations
├── tests/                  CTest-conformant unit and integration suites
├── third_party/            Vendored dependencies (git submodules)
├── wit/                    WIT interface definitions for WASM plugins
├── lua/                    lprowse and lprowsext Lua modules
├── plugins/                Native and WASM plugins
├── drivers/                Lua driver scripts
├── examples/               Example C++ and Lua applications
├── resources/              Runtime resources and manifests
└── scripts/                Developer and scaffolding scripts
```

Run `scripts/scaffold.sh` to create or refresh the build skeleton. The target
directory is `$PROWSETK_DIR`; when unset, the script falls back to the directory
one level above itself.
