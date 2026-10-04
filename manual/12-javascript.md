# Chapter 12: JavaScript

[Manual index](README.md)

## Page-script execution

QuickJS executes the loaded page's scripts in a persistent session context.
Flatworm installs its web-platform shim over host-mediated primitives. The shim
supplies familiar browser objects with explicitly restricted behavior.

```lua
session:load_html([[
  <h1 id="title">Before</h1>
  <script>document.querySelector('#title').textContent = 'After';</script>
]], 'https://example.test/')
assert(session:document():query_selector('#title'):text() == 'After')
assert(session:evaluate_js('document.querySelector("#title").textContent') == 'After')
```

This requires JavaScript enabled and a linked QuickJS runtime. Disable execution
through BrowserConfig, a consumed project/tool engine setting, or the CLI's
`--no-javascript`. Page JavaScript is separate from Lua automation.

## Web-platform surface

| Area | Implemented surface and restrictions |
|---|---|
| DOM | Queries, traversal, creation/mutation, attributes, HTML fragments, `classList`, `dataset`, inline `style` |
| Events | Capture/target/bubble dispatch, listeners, synthetic events, submit events, `MutationObserver` |
| Network | `fetch`, `Headers`, `Response`, sync/async `XMLHttpRequest`, `sendBeacon`, dynamic script/image requests |
| Navigation | `location`, links and forms queue bounded host navigation; history updates and same-document hashes update state |
| Lifecycle | `DOMContentLoaded`, `load`, microtasks and bounded timer flushing |
| Storage | `document.cookie`, `localStorage`, `sessionStorage` through host storage |
| Utilities | `URL`, `URLSearchParams`, navigator fields, console, text-encoding polyfills |
| Observers/layout | Inert intersection/resize observers, dummy canvas, no layout or WebGL |

There are no streaming response bodies, XHR progress/upload events, or browser
CORS enforcement. The URL and encoding polyfills are restricted implementations.
Inspect capabilities when a page depends on a particular feature.

Every script network operation uses the owning Session and NetworkClient:
cookies, redirects, plugins, interception, quotas imposed by the host, and
redaction apply. JavaScript never receives sockets or engine DOM pointers.

## Synthetic clicks

Managed Lua `element:click()` and C++ `Session::click_element(element)` execute
a cascade through the shim:

1. `pointerover`.
2. `pointerenter` (nonbubbling).
3. `pointerdown`.
4. `mousedown`.
5. `focus`, when the target is focusable and not already active.
6. `pointerup`.
7. `mouseup`.
8. `click`.

Submit controls also initiate a cancelable bubbling `submit` on their form
unless default processing was prevented. Network and queued navigation from
the action are drained before the host call returns. Repeated clicks on an
already active control omit the focus event.

```lua
local button = session:document():query_selector('button[type="submit"]')
assert(button and button:click())
```

SPA probing in scrape-endpoints uses this interaction path. Clicking controls
can execute their page behavior, including POSTs; configure the probe budget
and selection appropriately for the workflow.

## Controlled input

`element:type(text)` follows the native prototype value-setter path, enabling
framework setter wrappers and delegated input handlers to observe changes.
Typing focuses the control, emits keydown/keypress/input/keyup for the text,
then commits change and blur. Input events bubble and carry `insertText`.

```lua
local input = session:document():query_selector('input[name="search"]')
assert(input and input:type('Paris'))
```

`set_value` is direct property mutation and does not run that synthetic cascade.
Use typing when page handlers must run. Native form-control descriptors are
configurable for framework wrapping.

Interactability uses DOM heuristics: inline `display:none`,
`visibility:hidden`, `hidden`, disabled controls, and `aria-disabled="true"`
prevent interaction, including hidden ancestors. There is no coordinate hit
testing. The C++ session API returns false for unavailable/invalid interactions;
Lua exposes the corresponding success boolean. Check it explicitly.

## Limits and lifecycle

Default ScriptOptions are a 5,000-ms execution timeout, 16-MiB memory budget,
and 10,000 microtask jobs. Lifecycle flushes are bounded passes rather than an
unrestricted graphical-browser event loop. Host-mediated network time extends
the script deadline instead of consuming its CPU execution budget.

Handles are invalidated when the session installs another document. Do not
reuse an old page wrapper after navigation. Scripts that rely on missing
browser APIs can fail or leave only a partial DOM; query diagnostics and
capabilities before treating absent content as an empty real-world result.

Reference: `javascript_runtime.hpp`, `web_platform.cpp`, `web_platform_shim.hpp`.

## Native Flatworm modules

The standalone `include/Flatwork-Module.h` C ABI extends the page runtime with
native functions and constants. Its ABI version is `FLATWORM_MODULE_ABI_VERSION`
(currently 1), and a shared library exports `flatworm_module_entry()` returning
an immutable `FlatwormModuleDefinition`. Modules have their own host-selected
registry and per-runtime lifecycle, independent of browser plugins.
The header declares the entry point with C linkage, so C++ modules can define it
with `FLATWORM_MODULE_EXPORT` using the same spelling as the C example below.

### Loading and calling

```cpp
#include <prowsetk/browser.hpp>

prowsetk::Browser browser;
browser.modules().load_native("./build/default/examples/libflatworm_math.so");
auto session = browser.create_session();
session->load_html(R"html(
  <p id="answer"></p>
  <script type="module">
    import {add, pi} from 'flatworm:math';
    document.getElementById('answer').textContent = add(20, 22);
  </script>
)html", "https://example.test/");
// The paragraph contains 42.
auto answer = session->evaluate_js("Flatworm.module('math').add(20, 22)");
```

`Flatworm.module(name)` returns a frozen, null-prototype export object. The
global `Flatworm` bridge and its `module` function are read-only. The name must
be a string; no implicit object coercion is performed. ECMAScript scripts
import named exports from `flatworm:<name>`; dynamic `import()` uses the same
installed-only resolver. Both access paths share the native instance and
function objects in that runtime. Constants are read-only exports; a JSON
constant's nested contents are ordinary, mutable page-owned JavaScript values.

`JavaScriptRuntime::evaluate_module(source, options)` evaluates module source
with normal ScriptOptions and bounded promise-job checkpoints. Top-level
rejections that settle during the checkpoint propagate as ScriptResult errors.
As with classic script promises, unresolved top-level await is not implicitly
waited to completion. Static `<script type="module">` elements use this path;
their root `src` is fetched through Session/NetworkClient. Imported JavaScript
files, relative/network/file imports, module graphs and dynamic DOM-injected
module scripts are outside the current supported surface.

### C ABI recipe

This stateless native function validates its arguments and copies a scalar
result into the page:

```c
#include "Flatwork-Module.h"

static FlatwormStatus add(FlatwormCall* call) {
    FlatwormValue result = {0};
    if (call->argument_count != 2 ||
        call->arguments[0].type != FLATWORM_VALUE_NUMBER ||
        call->arguments[1].type != FLATWORM_VALUE_NUMBER)
        return FLATWORM_STATUS_INVALID_ARGUMENT;
    result.type = FLATWORM_VALUE_NUMBER;
    result.number = call->arguments[0].number + call->arguments[1].number;
    return call->api->set_result(call, &result);
}

static const FlatwormFunction functions[] = {{"add", 2, add}};
static const FlatwormModuleDefinition module = {
    FLATWORM_MODULE_ABI_VERSION, sizeof(FlatwormModuleDefinition),
    "math", "1.0.0", "Native arithmetic", NULL, NULL,
    functions, 1, NULL, 0
};
FLATWORM_MODULE_EXPORT const FlatwormModuleDefinition* flatworm_module_entry(void) {
    return &module;
}
```

Compile using `cc -std=c11 -fPIC -shared -Iinclude module.c -o build/module.so`.
Module libraries require only the ABI header, with no ProwseTk or QuickJS link.
The repository's `prowsetk_add_flatworm_module(target sources...)` CMake helper
sets up a MODULE library, header search path and project warnings. The installed
header is also self-contained. See
[`examples/flatworm-module`](../examples/flatworm-module/README.md) for a built
C library exporting `add`/`pi` and a C++ offline embedder.

| ABI item | Contract |
|---|---|
| Definition prefix | ABI version 1 and `struct_size >= sizeof(FlatwormModuleDefinition)` |
| Module name | ASCII `[A-Za-z_$][A-Za-z0-9_$-]*`, up to 64 bytes |
| Export name | Same identifier rule without `-`, up to 64 bytes; unique across functions/constants |
| Metadata | Required name/version; version up to 128 bytes, optional description up to 1024 bytes |
| Function | `name`, JavaScript `arity`/Function.length and `invoke(call)`; the callback validates actual argument count/types |
| Constant | `name` plus a typed FlatwormValue; metadata and bytes copied at definition load |
| Call | Borrowed host API, instance state and argument array; opaque `host_data` used only by the host API |
| Value | Undefined, null, boolean, double, UTF-8 bytes or JSON bytes; only the selected field is meaningful |
| Result | `call->api->set_result(call, &value)` copies immediately; default undefined; later successful calls replace it |
| Failure | Callback status becomes a generic TypeError, RangeError or InternalError; invalid `set_result` poisons the call even if ignored |

String transfers are length-delimited and allow embedded NULs. Structured
arguments use standard JSON serialization, including getters and `toJSON`;
cycles and BigInts can fail conversion. Functions, symbols and BigInts are
rejected as top-level arguments. JSON results are parsed under the JavaScript
memory/deadline budget. Native pointers and engine values never cross this ABI.
Getter/`toJSON` failures become generic TypeErrors without the thrown value.
Never retain a FlatwormCall, its arguments or host_data beyond the callback.

### Host ownership and lifecycle

| C++ interface | Purpose |
|---|---|
| `FlatwormModule::load_native(path)` | Explicitly load/validate shared-library entry and metadata |
| `FlatwormModule::from_static(definition)` | Copy an embedded definition; its callback code must outlive every runtime using it |
| `Browser::modules().load_native(path)` / `.register_static(definition)` | Select modules inherited by future sessions and standalone browser-created runtimes |
| Registry `.add(module)` / `.modules()` / `.remove(name)` | Add an owned definition, inspect a snapshot or remove future inheritance |
| `JavaScriptRuntime::install_module(module)` / `.modules()` | Install while idle or inspect installed metadata |

Loading definitions and inspecting capabilities never initialize native state.
Installing creates one instance in the target JavaScript runtime. Optional
`initialize(api, &instance)` and `shutdown(instance)` must be supplied together;
stateless definitions leave both null. The host API remains valid through
shutdown. Initialize is attempted once per installation, and shutdown is called
once after that attempt, including failed/throwing initialization with partial
or null state. Invalid export construction fails before initialization. Failed
installation publishes no exports and can be retried.

Closing a session from a console/network callback defers context teardown until
the active evaluation returns; subsequent document scripts are skipped.

Each runtime retains its module libraries. Removing a browser registry entry
does not invalidate captured functions, ES exports or queued jobs in existing
contexts. Session closure and runtime destruction remove the JavaScript
context/jobs before reverse-order instance shutdown and library release. Native
state persists across document loads along with the session's persistent
JavaScript context. Registries/runtimes follow the browser's serialized,
owning-thread execution model; the same native library can serve independent
instances in different sessions. Module authors must synchronize any process-
global state themselves and contain C++ exceptions at every ABI boundary.

### Bounds and support level

- 64 modules per registry/runtime, 256 total function/constant exports per module.
- 64 arguments per call, 32 nested calls through the native bridge.
- 1 MiB aggregate string/JSON argument bytes, separately 1 MiB result bytes,
  and 1 MiB aggregate constant bytes per module.
- JavaScript conversion, evaluation and microtasks use ScriptOptions limits.
- Native callbacks are trusted synchronous host code: JavaScript limits cannot
  preempt native execution or sandbox native allocations. Callbacks must not
  reenter the active runtime or open sockets for page requests. Normal page
  fetch/XHR continues through the owning Session and NetworkClient.

The bridge does not log argument/result bytes, native exception messages or
loader paths. Module callbacks still own the confidentiality of their own code
and state. The `javascript-modules` capability is
**Implemented with restrictions** with QuickJS, and **Unsupported** without it.
JavaScript-disabled sessions never initialize selected native modules. Native
module selection is explicit through the C++ host API; Prowse.toml and Lua do
not provide a library-loading entry point. Lua may use selected bindings through
its existing managed `session:evaluate_js` page evaluation.

Implementation: `src/flatworm/module.cpp`, `module_internal.hpp`,
`module_bindings.cpp`; coverage: `tests/unit/test_flatworm_modules.cpp`,
`tests/integration/test_flatworm_modules.cpp`, C ABI and C++ entry-linkage fixtures
in `tests/modules`.

### Module progression

The module surface reached its current shape in four steps, each independently
testable and each kept downstream of the layer below it:

1. **ABI and ownership.** `Flatwork-Module.h` fixed the version-1 contract:
   typed values, an opaque call context, per-runtime initialize/shutdown, and
   no engine, DOM, Lua or plugin types. Definition validation and library
   ownership moved into `src/flatworm/module.cpp`.
2. **Runtime integration.** `module_bindings.cpp` added QuickJS lowering,
   frozen export objects, argument/result/depth bounds and the installed-only
   `flatworm:` module loader. `JavaScriptRuntime` owns installation and
   `evaluate_module`, and `Browser::modules()` provides inheritance for future
   sessions.
3. **Embedder recipe.** A buildable C module and a C++ embedder in
   `examples/flatworm-module` fixed the loading recipe, and fixtures in
   `tests/modules` pinned ABI validation, ownership and exception containment.
4. **First shipped module.** `flatworm:rpc` proved the surface against a real
   protocol: it adds no engine privileges, needs only the ABI header and the
   C++20 standard library, and keeps transport in the session. Its integration
   coverage then exposed two engine-level defects that were fixed here too:
   session event cancellation now propagates to navigation and request paths
   (Chapter 13), and JSON-conversion getter/`toJSON` failures become generic
   value-free errors instead of echoing thrown argument text.

### Shipped JSON-RPC module

[`flatworm-modules/rpc`](../flatworm-modules/rpc/README.md) supplies JSON-RPC 2.0
helpers. `PROWSETK_BUILD_FLATWORM_MODULES=ON` builds `flatworm_module_rpc`;
load `build/default/flatworm-modules/rpc/libflatworm_rpc.so` through the browser
module registry before creating a session. No module is selected implicitly.

```javascript
import {request, parseResponse} from 'flatworm:rpc';
const message = request('add', [20, 22]);
const http = await fetch('/rpc', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(message)
});
if (!http.ok) throw new Error('RPC HTTP failure');
const response = parseResponse(await http.text(), message.id);
if (response.error) throw new Error('RPC remote failure');
document.getElementById('answer').textContent = response.result;
```

The frozen namespace also exports `notification`, `batch`, `result`, `error`,
`parseRequest`, `correlate`, standard error-code constants and protocol bounds.
Classic scripts access `Flatworm.module('rpc')`. Automatic IDs start at 1 per
runtime, persist across document loads, and do not advance for notifications or
explicit IDs. String IDs, null IDs and safe integer IDs are supported. Batch
correlation returns replies in call order, excludes notifications and rejects
duplicate, unexpected or missing IDs. A valid remote error envelope is returned
as data; the caller decides how to handle it.

The library uses only the ABI and C++20 standard library. Its native protocol
helpers build without QuickJS, while page execution requires QuickJS. HTTP
always uses session-mediated fetch/XHR, including request cancellation. Parsing
is strict, with 128 messages per batch, 64 JSON levels below the root, 16,384
values and the ABI's 1-MiB argument/result bounds. Raw wire text lets validation
reject duplicate keys and fractional IDs before JavaScript parsing can discard
or round them. Malformed data and bound failures use value-free errors.
The module README documents each export, numeric/UTF-8 restrictions,
notification-only replies and payload ownership. Its offline example is
`flatworm_rpc_example`; tests are `tests/unit/test_rpc_module.cpp` and
`tests/integration/test_rpc_module.cpp`.

Shipped modules are grouped under `flatworm-modules/` with a per-module README;
see [`flatworm-modules/README.md`](../flatworm-modules/README.md) for the index
and [`flatworm-modules/rpc/README.md`](../flatworm-modules/rpc/README.md) for
the full export table. Set `PROWSETK_BUILD_FLATWORM_MODULES=OFF` to omit them
and their suites; the ABI, examples and other chapters are unaffected.

**Next:** [Events and diagnostics](13-events-and-diagnostics.md).
