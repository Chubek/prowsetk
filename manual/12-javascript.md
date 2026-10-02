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

**Next:** [Events and diagnostics](13-events-and-diagnostics.md).
