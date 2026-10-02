# Chapter 30: eBPF Interface

[Manual index](README.md)

## Two integration surfaces

ebpf-interface provides a bounded host-mediated endpoint collector and an
optional libbpf runtime facade. The collector works through ordinary ProwseTk
request/response hooks even when kernel eBPF support is disabled.

The native shared library is `libprowsetk_ebpf_interface.so`. Build it with the
normal preset; enable libbpf integration explicitly:

```sh
cmake --preset default -DPROWSETK_ENABLE_EBPF=ON
cmake --build --preset default
```

Dependency discovery uses a linkable libbpf installation through
`cmake/Dependencies.cmake`. A disabled facade remains available without it.
Actual object loading/attachment also depends on the host kernel, program type,
and process capabilities.

## Host endpoint observations

```cpp
#include <prowsetk/plugins/ebpf_interface.hpp>
namespace ebpf = prowsetk::plugins::ebpf_interface;
ebpf::EndpointCollector collector;
collector.observe_request("GET", "https://example.test/api/items");
collector.observe_response("https://example.test/api/items", 200,
                           "application/json");
const auto json = collector.render_json();
```

EndpointObservation contains method, URL, status, and content type. The
collector's default capacity is 4,096 records, with redaction enabled.
Additional records above the bound are not appended. Responses update the
latest matching pending URL observation; a response without a pending entry
can create a GET record. `clear()` resets the collection.

Native configuration keys include `ebpf-interface.output` (alias `ebpf.output`)
and `ebpf-interface.max_records`. Load, initialize, and configure through
PluginRegistry. Its request hook records the call; its response hook updates
the record and writes configured JSON output. That hook output is an ordinary
file write, not the daemon tools' transactional cache.

```json
{"observations":[{"method":"GET","url":"https://example.test/api/items","status":200,"content_type":"application/json"}]}
```

Observations are evidence of calls seen by this host, rather than inferred full
API schemas. Feed selected data into discovery/enrichment through documented
interfaces when building a specification.

## Runtime facade

Runtime exposes `available`, `capabilities`, `load_object`, `attach_all`,
`map_names`, `poll`, and `detach`. C++ owns all libbpf handles; Lua receives none.

```cpp
#include <prowsetk/plugins/ebpf_interface.hpp>
namespace ebpf = prowsetk::plugins::ebpf_interface;
ebpf::Runtime runtime;
std::string error;
if (runtime.available() && runtime.load_object("build/tracer.bpf.o", &error)) {
    if (!runtime.attach_all(&error)) {
        // Handle the categorical loader/attachment failure.
    }
    const auto maps = runtime.map_names();
    runtime.detach();
}
```

Without libbpf, capability booleans are false and loading reports the disabled
build. With libbpf, the facade can open/load an object, attach its programs, list
maps, and release links/object through RAII. Availability reports compiled
support, not a successful kernel attachment.

The capability record names libbpf, object, program, map, link, ring buffer, and
perf buffer surfaces. The current `poll` implementation returns zero and does
not deliver kernel samples. No shipped object automatically turns this facade
into an HTTP tracer; configure/implement the observation producer explicitly.

## Lua helper and redaction contract

`plugins/ebpf-interface/lua/ebpf_interface.lua` supplies `normalize_spec`, a
capability-name list, and `render_observations(endpoints)`. It is a Lua
specification formatter, not a kernel/IPC binding. Its capabilities list is not
an availability check. Its formatter uses supplied method/URL fields and leaves
status/content type empty/default.

The collector has its own restricted query-name redaction for token,
access_token, api_key/apikey, password, secret, session/sessionid, and auth.
It does not implement the core Redactor's complete encoded-name/userinfo/
fragment policy. Supply already redacted URLs through `prowsetk::Redactor`
when that policy is required. The Lua formatter also expects sanitized inputs.
Cookies, authorization headers, and request/response bodies are not observation
fields.

The plugin never opens a page network socket; page requests remain mediated by
Session/NetworkClient. libbpf is optional instrumentation within the host layer,
with actual support and producer coverage reported honestly.

Reference: `plugins/ebpf-interface/include/prowsetk/plugins/ebpf_interface.hpp`,
`plugins/ebpf-interface/src/ebpf_interface.cpp`, native `plugin_entry.cpp`.

**Return to:** [Manual index](README.md).
