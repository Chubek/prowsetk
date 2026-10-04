# Native Flatworm modules

These shipped modules extend Flatworm's page JavaScript through the independent
version-1 ABI in [`include/Flatwork-Module.h`](../include/Flatwork-Module.h).
The C++ host explicitly selects libraries using `Browser::modules()` or
`JavaScriptRuntime::install_module`.

| Module | Import | Purpose |
|---|---|---|
| [rpc](rpc/README.md) | `flatworm:rpc` | Bounded JSON-RPC 2.0 envelopes, validation and batch correlation |

`PROWSETK_BUILD_FLATWORM_MODULES=ON` (the default) builds these libraries.
Installing a library does not select it for page execution. See
[Manual Chapter 12](../manual/12-javascript.md) for the ABI, ownership and runtime
support contract, and [`examples/flatworm-module`](../examples/flatworm-module/README.md)
for a minimal C module.
