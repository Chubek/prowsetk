#ifndef PROWSETK_WASM_RUNTIME_HPP
#define PROWSETK_WASM_RUNTIME_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "prowsetk/capability.hpp"

namespace prowsetk {

// Per-plugin sandbox configuration. Defaults are safe: WASI off, no
// filesystem, host-mediated network only (README "WASI policy").
struct WasmSandboxConfig {
    bool component_model = true;
    bool wasi = false;
    std::size_t max_memory_bytes = 128u * 1024u * 1024u;
    std::uint32_t max_table_elements = 10000;
    std::uint32_t execution_timeout_ms = 30000;
    std::uint64_t fuel = 10000000;
};

class WasmModule {
public:
    virtual ~WasmModule() = default;
    virtual std::string name() const = 0;
};

class WasmInstance {
public:
    virtual ~WasmInstance() = default;
    virtual bool ok() const = 0;
    virtual std::string error() const { return {}; }
};

// The only place Wasmtime may be named. The rest of ProwseTk depends solely on
// this interface so the runtime remains replaceable.
class WasmRuntime {
public:
    virtual ~WasmRuntime() = default;

    virtual bool enabled() const = 0;
    virtual CapabilitySet capabilities() const = 0;

    virtual std::shared_ptr<WasmModule> load_module(
        const std::filesystem::path& path) = 0;
    virtual std::shared_ptr<WasmInstance> instantiate(
        const WasmModule& module, const WasmSandboxConfig& config) = 0;
};

// Returns a disabled runtime unless ProwseTk is built with WASM support.
std::unique_ptr<WasmRuntime> make_wasm_runtime();

}  // namespace prowsetk

#endif  // PROWSETK_WASM_RUNTIME_HPP
