# Chapter 11: Lua Extensions

[Manual index](README.md)

## Native modules and reusable Lua code

LuaRuntime supplies four principal native modules: `lprowse` for browser
control, `lprowsext` for extraction/extensions, `lprowseir` for intermediate
representations, and `lpdql` for query projections. `lprowsext.dom`,
`lprowsext.endpoints`, `lprowsext.extractor`, and `lprowsext.wasm` can also be
required individually.

The host provides these native modules. Plugin Lua files are ordinary reusable
modules and need an explicit file/module search path. From the repository root:

```lua
local scrape = dofile('plugins/scrape-endpoints/lua/scrape_endpoints.lua')
local login = dofile('plugins/ezlogin/lua/ezlogin.lua')
```

For an installation with prefix `/opt/prowsetk`, the corresponding files are
under `/opt/prowsetk/share/prowsetk/plugins/scrape-endpoints/` and `ezlogin/`.
Use `dofile` with that location, or configure `package.path` for your project's
module naming scheme. Loading a native shared plugin's metadata does not
automatically install a Lua module with the advertised name.

## Explicit document processors

```lua
local ext = require('lprowsext')
ext.register_document_processor('headings', function(document)
    return document:xpath_strings('//h1/text()')
end)

local results = ext.process_document(session:document())
assert(type(results.headings) == 'table')
```

Processors are keyed by name within the runtime; registering the same name
replaces its callback. `get_document_processor(name)` retrieves the function
or nil. `process_document(document)` invokes the current registry explicitly
and returns a table keyed by processor name. Successful false, numeric, string,
and table values are retained; nil results and failed callbacks are omitted.
Iteration order is unspecified. Registration alone does not subscribe to
navigation.

When a particular processor's failure must abort the workflow, call the
retrieved function directly or use a separately checked `pcall`. The aggregated
processor API intentionally omits failures.

## Automatic document extractors

```lua
local ext = require('lprowsext')
local browser = require('lprowse').browser.new()
local extractor = ext.extractor.new()
extractor:on_document(function(document)
    -- Keep the callback and its output inside the extension's lifecycle.
    return { title = document:title() }
end)
browser:install_extension(extractor)

local active = browser:create_session()
active:load_html('<title>Local report</title><h1>Example</h1>')
local data = extractor:run(active:document())
assert(data.title == 'Local report')
```

Installing an extractor subscribes it to `document_created` notifications on
that browser. Keep the extractor reachable for as long as its callback should
run: collecting it makes the callback inert and unregisters it at runtime
teardown, as described in [Chapter 9](09-lua-control-api.md). Automatic callback
return values are not an output archive; store needed values in extension-owned
state. Automatic callback errors are suppressed by this notification path.
Explicit `extractor:run(document)` returns its result and propagates callback
errors; without a callback it returns nil.

Document callbacks operate on the document as it exists at the notification
stage. Run explicit extraction after navigation/interactions when you need
post-script state. DOM objects passed without a managed session can be inspected
and mutated, but synthetic session interactions require a session-backed handle.

## DOM, endpoint, and WASM helpers

`ext.dom.xpath(document_or_session, expression)` returns a node array or XPath
scalar. `xpath_strings` preserves selected text/attribute values.
`ext.endpoints.extract(document_or_session, options)` returns a managed
extraction result; Chapter 15 describes observation and provenance.

```lua
local wasm = require('lprowsext.wasm')
assert(wasm.available() == false) -- current runtime adapter
local module, diagnostic = wasm.load('plugin.wasm')
assert(module == nil and type(diagnostic) == 'string')
```

WASM availability is independent of the existence of a WIT file or a parsed
plugin declaration. Chapter 21 distinguishes the contract from the current
disabled runtime.

## Host-specific extension modules

| Module | Host | Chapter |
|---|---|---|
| `lcrawler` | Standalone crawler or its driver adapter | 24 |
| `lpgwatch` | Pagewatch worker | 25 |
| `lspider` | Spider driver invocation | 26 |
| Plugin Lua helpers | Explicitly loaded driver/extension files | 16–20, 27, 30 |

Generic `[[extensions]]` and `[lua].preload` settings are parsed configuration
data. A host must explicitly load the files and arrange callbacks. Trusted Lua
extensions can use the standard libraries available to the process; process
watchdogs in specialized tools do not make those scripts a security sandbox.

Reference: [Lua module notes](../lua/README.md), `src/core/lua_runtime.cpp`.

**Next:** [JavaScript](12-javascript.md).
