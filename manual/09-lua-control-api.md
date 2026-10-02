# Chapter 09: Lua Control API

[Manual index](README.md)

## Runtime and browser creation

`require('lprowse')` resolves to the native module registered by LuaRuntime.
The standalone files in `lua/` are surface mirrors; a stock Lua interpreter
does not acquire a Flatworm browser by loading them.

```lua
local prowse = require('lprowse')
local browser = prowse.browser.new()
local active = browser:create_session()
active:load_html('<h1>Lua controls the page</h1>', 'https://example.test/')
assert(active:document():query_selector('h1'):text() == 'Lua controls the page')
```

Inside a host-bound runtime, no-argument `browser.new()` returns the host's
browser. Passing a configuration table creates a new browser using inherited
configuration and copied cookies; a custom host transport is not transferred.
Crawler/pagewatch/spider scripts should use the supplied host session to retain
their transport policy.

Browser configuration keys read by the Lua constructor include `javascript`,
`follow_redirects`, `max_redirects`, `timeout_ms` (and legacy `timeout`, also in
milliseconds), `user_agent`, `observe_network`, and `unsupported_api_behavior`.

## Session reference

| Method | Result / behavior |
|---|---|
| `navigate(url)` | Navigate and install a document |
| `load_html(html, base_url)` | Install supplied HTML |
| `document()`, `current_url()` | Current managed document and URL |
| `request(method, url, options)` | Response table: status, body, final URL, headers |
| `evaluate_js(script)` | Page-JS evaluation result |
| `set_header(name, value)`, `headers()`, `clear_headers()` | Manage default headers |
| `import_cookies_json(path)` | Import browser-exported cookies into the scoped jar |
| `on(event, callback)`, `off(subscription)` | Session-scoped subscriptions |
| `capabilities()` | Classification table with `has(name)` helper |
| `click_element(element)`, `type_element(element, text)` | Host synthetic interactions |
| `close()` | Close the session |

`request` options include `headers` and `body`; requests inherit the owning
browser's timeout and response-size limit. Response bodies and headers are raw
data, so extract the needed fields before logging or exporting.

```lua
local response = active:request('GET', 'https://example.com/', {
    headers = { Accept = 'text/html' }
})
if response.status >= 200 and response.status < 300 then
    active:load_html(response.body, response.final_url)
end
```

## Document and element reference

Documents expose `title`, `url`, `text`, `html`, `root`, `metadata`, `links`,
`forms`, `scripts`, `resource_urls`, `create_element`, `get_element_by_id`,
`get_elements_by_tag_name`, `query_selector`, `query_selector_all`, `xpath`,
and `xpath_strings` as methods.

Elements expose `tag_name`, `id`, `class_name`, `attribute`, `attributes`,
`has_attribute`, `set_attribute`, `remove_attribute`, `text`, `inner_html`,
`html` (outer HTML), `value`, `set_value`, query methods, `matches`, `xpath`,
`children`, `parent`, `first_child`, sibling methods, `append_child`,
`remove_child`, and `set_text`. Managed session elements also expose `click`
and `type`.

Lists are one-based Lua arrays. Missing selections return nil. Attribute
lookups commonly return an empty string for a missing attribute; use
`has_attribute` when absence must be distinguished from an empty value.

## URL utilities and errors

`prowse.url.resolve(base, reference)` resolves links. `normalize(url)` normalizes
and strips fragments for crawler deduplication. `origin(url)` accepts HTTP(S)
origins without userinfo. `redact(url)` removes private URL components according
to the default URL policy.

```lua
local prowse = require('lprowse')
local ok, value = pcall(function()
    return prowse.url.origin('https://example.test/path')
end)
assert(ok and value == 'https://example.test')
```

## Managed handle lifetime

Session, document, element, and extractor values are managed userdata. They own
references to the corresponding C++ objects and never expose engine pointers.

Collecting a handle is a supported way to end its lifecycle. The finalizer first
invalidates the callbacks the handle owns, so a collected session or extractor
stops receiving events, and then releases the shared lifetime guard that those
subscriptions referenced. A collected handle's dispatcher entry is not removed
at that point; it becomes inert, and runtime teardown removes it along with every
other subscription and its registry reference.

A browser created by `prowse.browser.new()` is owned by Lua, and sessions refer
to their browser by pointer. Releasing such a browser is deferred while any of
its sessions are still alive, so the browser handle may be dropped before them:

```lua
local session = require('lprowse').browser.new():create_session()
session:load_html('<p>x</p>')   -- the browser is still alive here
```

The browser is deleted once its last session is finalized, including through a
document or element handle that still holds that session. A browser supplied by
the host through `bind_browser` is never owned by Lua and is never destroyed by
collection.

This means a subscription is only effective while its owning handle is
reachable:

```lua
local prowse = require('lprowse')
local browser = prowse.browser.new()
local events = 0

local driver = browser:create_session()
driver:on('document_created', function() events = events + 1 end)
driver:load_html('<p>first</p>')            -- events == 1

do
    local watched = browser:create_session()
    watched:on('document_created', function() events = events + 100 end)
    watched:load_html('<p>second</p>')      -- events == 101 while reachable
end
collectgarbage('collect')
collectgarbage('collect')

driver:load_html('<p>third</p>')            -- events == 102, not 202
```

Retain a handle in a variable, table, or upvalue for as long as its callbacks
must run. Native API errors raise Lua errors. C++ embedding reports a failed
LuaResult when a top-level call fails. Keep the host Browser and bound Session
alive for the runtime's use.

Generic LuaRuntime opens Lua's standard libraries and does not enforce a
per-call instruction or memory budget. Specialized daemon hosts add their own
bounded instruction/process-watchdog policies, described in the tool chapters.

Reference: `src/core/lua_runtime.cpp`, `lua/lprowse/init.lua`, Lua integration tests.

**Next:** [Drivers](10-drivers.md).
