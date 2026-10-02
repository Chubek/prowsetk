# Chapter 05: C++ Embedding

[Manual index](README.md)

## Minimal application

```cpp
#include <iostream>
#include <prowsetk/browser.hpp>
#include <prowsetk/error.hpp>

int main() {
    try {
        prowsetk::BrowserConfig config;
        config.javascript = false;
        prowsetk::Browser browser(config);
        auto session = browser.create_session();
        session->load_html("<h1>Hello</h1><a href='/next'>Next</a>",
                           "https://example.test/");
        auto document = session->document();
        std::cout << document->query_selector("h1")->text() << '\n';
        for (const auto& link : document->links()) {
            std::cout << link->attribute("href") << '\n';
        }
        return 0;
    } catch (const prowsetk::Error& error) {
        std::cerr << prowsetk::to_string(error.code()) << '\n';
        return 1;
    }
}
```

The sample uses no network. `Session::document()` and selectors return managed
shared pointers; an absent selection is a null pointer. Check optional nodes
before dereferencing them in general-purpose code.

## Linking

For an installed package:

```cmake
cmake_minimum_required(VERSION 3.25)
project(InspectPage LANGUAGES CXX)
find_package(ProwseTk CONFIG REQUIRED)
add_executable(inspect main.cpp)
target_compile_features(inspect PRIVATE cxx_std_20)
target_link_libraries(inspect PRIVATE ProwseTk::core)
```

Configure with `-DCMAKE_PREFIX_PATH=/path/to/install`. `ProwseTk::session` is
also exported for session/storage support. Within the repository use the same
namespaced aliases. Plugin static helper libraries have their own build-tree
availability and are not all exported as installed embedding libraries.

## Ownership and responsibilities

| Component | Responsibility |
|---|---|
| `Browser` | Configuration, transport, storage, events, plugins, runtime factories |
| `Session` | URL/document lifecycle, headers, requests, script context, interactions |
| `Document` / `Element` | DOM inspection and mutation |
| `NetworkClient` | Actual transport; replaceable through `set_network_client` |
| `LuaRuntime` / `JavaScriptRuntime` | Automation and page scripting, respectively |
| `Storage` / `EventDispatcher` | Storage interfaces and notifications |
| `WebPlatform` / `CapabilitySet` | Declared web API behavior |

Keep a Browser alive while its sessions and bound runtimes are used. C++ element
wrappers retain their DOM tree, but an element from an old document is not a
valid interaction target for the newly installed session page. Lua and page-JS
handles add host lifetime checks. `close()` releases the session's active state.

## Compact embedding and IR delivery

`EmbeddedBrowser` owns one Browser and Session and provides convenient loading
and canonical event streaming:

```cpp
#include <prowsetk/embedding.hpp>

prowsetk::EmbeddedBrowser browser;
browser.load_html("<p>Report</p>");
const auto result = browser.stream_events([](const prowsetk::ProwseEvent& event) {
    // Consume the start / attribute / text / end record.
    return prowsetk::EventStreamControl::Continue;
});
```

Returning `Stop` successfully stops delivery while retaining the document.
The result records delivered count and completeness. An unloaded document or
an empty visitor reports the normal error taxonomy.

## Embedding Lua

Construct `LuaRuntime`, call `bind_browser(&browser)`, optionally
`bind_session(session)`, then `run`, `run_file`, or `call_function`. Inspect
`LuaResult::ok`; `last_error()` provides the diagnostic. `bind_session` supplies
the managed global `session`. A bound browser's no-argument `lprowse.browser.new()`
returns that host browser, preserving its transport policy.

The C-compatible PDQL interface is documented in Chapter 8; the native plugin
C ABI is documented in Chapter 21. Neither boundary transfers C++ objects.

Reference: `browser.hpp`, `embedding.hpp`, `lua_runtime.hpp`, `error.hpp`.

**Next:** [Sessions and networking](06-sessions-and-networking.md).
