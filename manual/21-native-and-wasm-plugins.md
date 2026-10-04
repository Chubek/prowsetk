# Chapter 21: Native and WASM Plugins

[Manual index](README.md)

Flatworm's native page-JavaScript bindings use the independent
`Flatwork-Module.h` ABI and module registry, covered in
[Chapter 12](12-javascript.md#native-flatworm-modules).

## Registry lifecycle

The optional [basic-gui](../plugins/basic-gui/README.md) plugin supplies an FLTK
desktop inspector. `cmake --preset gui` enables its graphical adapter;
`ptk-basic-gui --file plugins/basic-gui/example.html` opens the offline demo.
Native loading remains display/network-free and retains the version-2 C ABI.
Opening a live viewer is an explicit C++ `basic_gui::Viewer(session)` operation.
The core/default build and the plugin's controller/projection tests remain
headless. Preview clicks and typing use Session synthetic interactions;
networking, cookies, TLS and request hooks stay in the host engine.

PluginRegistry owns loaded libraries and descriptors. Loading, initializing,
configuring, executing hooks, and shutting down are distinct stages:

```cpp
auto& plugins = browser.plugins();
plugins.load_native("/opt/prowsetk/lib/prowsetk/plugins/libprowsetk_scrape_endpoints.so");
plugins.initialize_all();
plugins.configure_all({
    {"scrape-endpoints.output_openapi", "build/openapi.yaml"},
    {"scrape-endpoints.output_postman", "build/postman.json"}
});
// Session loading now dispatches supported initialized document hooks.
```

`plugins()` returns descriptor copies; `find(name)` and `has_capability(name)`
inspect registrations. `discover(directory, warnings)` scans native shared
libraries, `.lua`, and `.wasm` files and collects individual loading failures.
`shutdown_all` runs shutdown callbacks and releases libraries.

`load_lua` records metadata; the LuaRuntime must execute the file explicitly.
`load_wasm` records its sandbox settings. A plugin's advertised Lua module or
capability name does not itself create a callable native Lua binding.

## Native C ABI

Include `prowsetk/ProwseTk-Plugin.h`. The current ABI version is 2, with entry
symbol `prowsetk_plugin_entry` returning a pointer to a static ProwseTkPlugin.
A minimal C plugin can be written as:

```c
#include "prowsetk/ProwseTk-Plugin.h"

static const ProwseTkPluginInfo metadata = {
    "example", "0.1.0", "2", "Example native plugin",
    PROWSETK_PLUGIN_NATIVE, NULL, NULL, NULL, 0
};
static const ProwseTkPluginInfo *info(void) { return &metadata; }
static int initialize(ProwseTkHost *host) {
    return host && host->api &&
           host->api->abi_version == PROWSETK_PLUGIN_ABI_VERSION
        ? PROWSETK_STATUS_OK : PROWSETK_STATUS_INVALID_ARGUMENT;
}
static const ProwseTkPlugin plugin = {
    initialize, NULL, info, NULL, NULL, NULL, NULL
};
PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin *prowsetk_plugin_entry(void) {
    return &plugin;
}
```

For a Linux development build, save it as `example-plugin.c` and run
`cc -std=c11 -fPIC -shared -I include example-plugin.c -o build/example-plugin.so`.
The ABI header is self-contained and does not require linking C++ for this
metadata-only example.

| Callback | Contract |
|---|---|
| `initialize(host)` | Validate host version and set up plugin state |
| `shutdown(host)` | Release plugin-owned state |
| `info()` | Return stable metadata/capability strings |
| `configure(host, entries, count)` | Consume borrowed key/value configuration |
| `before_request` | Continue, reject, or supply a replacement request |
| `after_response` | Continue or reject a response |
| `on_document` | Inspect borrowed URL/title/HTML/text snapshot |

The host exposes logging, event emission, and redaction through ProwseTkHostApi.
The ABI does not pass a C++ Session/DOM, a socket, or a direct network-request
service. Live plugin helpers therefore operate through separate documented
C++/Lua Session APIs.

## Ownership, errors, and ordering

Input strings, arrays, bytes, host services, and snapshots are borrowed. Do not
free them or retain a document snapshot after the callback. Plugin metadata
and returned replacement-request storage must remain valid for the host's
immediate consumption; never return pointers to stack-local storage.

Callbacks return integer status codes: OK, error, invalid argument, or security
violation. Catch C++ exceptions inside exported callbacks. The registry also
converts callback failures into host Errors; no exception may cross the C ABI.
Use RAII/value-owned state on the C++ side.

Request hooks run in registration order, so a replacement can be observed by
later hooks. Session dispatch places them at the host request/response boundary.
The log sink is synchronous and must not reenter the registry. A registry is
not an automatic scheduler for concurrent plugin execution; arrange threading
and callback lifetimes in the host.

## WASM contracts and current availability

New portable plugin interfaces use WIT and the Component Model. The contract
files under `wit/` describe small request/response, observation, extraction,
and plugin-specific interfaces. They are contracts for runtime integration,
not evidence that the runtime executes components today.

The current make_wasm_runtime adapter returns a disabled implementation even
with `PROWSETK_ENABLE_WASM=ON`. C++ `enabled()` and Lua `wasm.available()`
report false. Lua `load` returns nil/diagnostic. The `wasm` preset verifies this
disabled-path contract.

WasmSandboxConfig defaults are Component Model enabled, WASI disabled,
128-MiB memory, 10,000 table elements, 30,000-ms execution timeout, and
10,000,000 fuel. Network access is designed to remain host-mediated;
filesystem/environment/socket grants require explicit runtime implementation
and host policy. Declarative configuration cannot enable capabilities in an
absent backend. Lua never receives raw runtime handles.

For new plugins, document actual hooks, install paths, Lua loading, capability
levels, redaction, and bounded CTest coverage. Add dependencies centrally in
`cmake/Dependencies.cmake` and update the dependency inventory.

Reference: `ProwseTk-Plugin.h`, `plugin_registry.hpp`, `wasm_runtime.hpp`, `wit/`.

## AI oracle service

`plugins/ai-oracle` provides explicit OpenAI Responses API inquiries for other
plugins and drivers. It uses OpenAIpp's wire/header helpers through the host
NetworkClient, and ships an exported `ProwseTk::ai_oracle` C++/C library, an
ABI-v2 native shared plugin with opaque C service symbols, and a callable
`ai_oracle` Lua module. The native registry facade is network-free; each oracle
client owns its explicit enablement, key, model, endpoint, and budgets.

Lua modules are installed in `lib/prowsetk/lua`; add that directory to
`LUA_CPATH` before `require("ai_oracle")`. `new(options, send_callback?)` creates
a client; `ask`, `captcha`, and `crawl` return advisory results or
`nil, message, error_code`. The default transport is the core NetworkClient;
a callback supports host-selected API sessions/policies and hermetic tests.

Detached HTML/context redaction, embedded PNG/JPEG input, JSON-object output,
request/response/time/token/attempt bounds, and rejected redirects are part of
the supported contract. The caller applies recommendations through existing
automation interfaces and checks actual CAPTCHA/login/crawl outcomes. See the
[plugin reference](../plugins/ai-oracle/README.md) for complete configuration,
ownership, transport requirements, examples, and limits.

## OpenCode marionette

`plugins/opencode-marionette` exposes explicit C++ session control and the
`ptk-opencode-marionette` executable. Its bounded version-1 JSON policy names
permitted click/type/navigation actions; OpenCode chooses an ID, and Flatworm
executes it with same-origin request and finite action/probe budgets. Discovery
accumulates through scrape-endpoints and schema-grabber, exporting heuristic
OpenAPI/Postman schemas with incomplete coverage metadata. The ABI-v2 facade
loads without network traffic and does not automatically drive pages. See the
[plugin guide](../plugins/opencode-marionette/README.md) for configuration and limits.
The runner defaults to OpenCode V2 `/api` prompt/message polling with tools
denied for its decision session. `scripts/run-scrape-booking.sh` supplies a
trusted Booking policy and prepares the authenticated session with the existing
driver before controlling it; both exports contain schema enrichment.
Offline HTML uses an in-memory page transport and zero GET probes, while
OpenCode remains a separate explicit connection.

**Next:** [Intermediate representations](22-intermediate-representations.md).
