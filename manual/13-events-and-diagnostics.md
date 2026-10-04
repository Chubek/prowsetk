# Chapter 13: Events and Diagnostics

[Manual index](README.md)

## Event subscription

EventDispatcher delivers mutable C++ Event values synchronously. An Event
contains its type, URL, name, message, attributes, optional typed payload,
cancellation flag, and originating session ID. Keep callbacks short and avoid
recursively navigating from a processing hook.

```cpp
const auto subscription = browser.events().subscribe(
    prowsetk::EventType::BeforeNavigation, [](prowsetk::Event& event) {
        if (event.url.starts_with("http://")) event.cancelled = true;
    });
// ... perform browser work ...
browser.events().unsubscribe(subscription);
```

Cancellation is honored at the pre-operation stages a session dispatches:
`before_navigation`, `before_request`, and `before_redirect`. A cancelled
navigation returns without fetching or replacing the installed document, a
cancelled request throws `SecurityViolation` before transport and without
emitting `after_response`, and a cancelled redirect returns the response that
carried the `Location` header without following it. Cancellation flags set on
browser-scope events, such as a `Browser::handle_unsupported_api` warning, have
no operation to cancel. Native plugin request hooks provide explicit
replacement/rejection contracts. Do not assume that an arbitrary event-field
edit rewrites a request.

Lua subscriptions receive event-table snapshots:

```lua
local failures = 0
local id = session:on('script_exception', function(event)
    failures = failures + 1
end)
-- ... load or interact with the page ...
session:off(id)
```

Names use snake case. `session:on('all', callback)` subscribes broadly; events
from other identified sessions are filtered, while browser-global events can
still reach the callback. Editing the Lua table does not mutate the C++ event.
Lua event callback errors are suppressed in this notification path. Use an
explicit checked operation when failure must terminate automation.

## Event names

| Group | Names |
|---|---|
| Sessions | `session_created`, `session_destroyed` |
| Navigation | `before_navigation`, `after_navigation`, `before_redirect` |
| Transport | `before_request`, `after_response` |
| DOM | `document_created`, `dom_mutation` |
| Scripts | `before_script`, `after_script`, `script_exception`, `console` |
| State | `cookie_change`, `storage_access` |
| Plugins | `plugin_init`, `plugin_shutdown` |
| Discovery | `anti_bot_detected`, `endpoint_discovered` |
| Compatibility | `unsupported_api` |

The C++ `to_string(EventType)` and `parse_event_type` implement this mapping.
DOM mutation attributes identify the mutation; storage instrumentation reports
store/key/operation rather than stored values.

## Capabilities and unsupported APIs

```lua
local capabilities = session:capabilities()
local fetch_level = capabilities.fetch
if capabilities:has('fetch') then
    -- Check fetch_level for the restrictions relevant to this workflow.
end
```

Classification strings are `fully-implemented`, `partially-implemented`,
`implemented-with-restrictions`, `dummy-implementation`, and `unsupported`.
`has` is true for every classification except unsupported, including a dummy
implementation. Inspect the level and C++ capability notes when meaningful
behavior matters.

`Browser::handle_unsupported_api` follows `unsupported_api_behavior`: `warn`
emits a diagnostic event; `exception` and `abort` throw Unsupported; `default`
and `dummy` continue silently. This policy concerns reported unsupported uses;
it does not implement arbitrary missing web APIs.

## Errors and redaction

C++ throws `prowsetk::Error` with an ErrorCode: invalid argument/URL, parsing,
not found, unsupported, networking, timeout, redirects, resource/security,
plugin/WASM/Lua/JavaScript/storage/I/O, or internal failure. Lua operations
normally raise errors; LuaRuntime returns LuaResult. The C PDQL boundary returns
NULL and diagnostics; plugin ABI calls return status codes.

Redactor replaces sensitive header/query values, understands percent-encoded
query names, and removes URL userinfo and fragments. Default-sensitive names
include authorization/cookie credentials and token/password/API-key names.
PDQL and daemon output additionally sanitize structural DOM fields. Direct DOM,
response, JavaScript result, and cookie getters remain raw application data.

## Troubleshooting by stage

| Symptom | First checks |
|---|---|
| HTTPS unavailable or certificate failure | OpenSSL configuration, CA trust, hostname, proxy |
| Query parse error | Supported CSS/PDQL syntax, valid XPath, pugixml availability |
| Page stays empty or has no expected control | HTTP status, scripts/QuickJS, script exceptions, unsupported APIs |
| Interaction returns false | Active document, hidden/disabled ancestors, JavaScript availability |
| Driver module not found | Native runtime binding, plugin Lua path, project root |
| Watcher/spider stops | Status/error/watchdog counters, transport quotas, explicit resume and authentication |
| Missing endpoint/schema | Selection/filter budgets, observed versus static provenance, response bytes |

Use the finite bounds in the relevant chapter to distinguish rejected input
from unsupported behavior. Test a minimal offline document, then a hermetic
MemoryNetworkClient response, before adding live networking.

Reference: `event.hpp`, `error.hpp`, `capability.hpp`, `redaction.hpp`.

**Next:** [Cookies and storage](14-cookies-and-storage.md).
