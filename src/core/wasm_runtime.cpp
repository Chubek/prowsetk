#include "prowsetk/wasm_runtime.hpp"

#include "prowsetk/error.hpp"

namespace prowsetk {
namespace {

// Returned whenever ProwseTk is built without a usable WASM engine. Keeps the
// rest of the toolkit independent of Wasmtime and safe by default.
class DisabledWasmRuntime : public WasmRuntime {
public:
    bool enabled() const override { return false; }

    CapabilitySet capabilities() const override {
        CapabilitySet capabilities;
#if defined(PROWSETK_ENABLE_WASM)
        capabilities.set("wasm", ImplementationClass::Unsupported,
                         "PROWSETK_ENABLE_WASM=ON but no WASM engine is linked");
#else
        capabilities.set("wasm", ImplementationClass::Unsupported,
                         "no WASM runtime is linked into this build");
#endif
        capabilities.set("wasm.component-model",
                         ImplementationClass::Unsupported);
#if defined(PROWSETK_ENABLE_WASI)
        capabilities.set("wasi", ImplementationClass::Unsupported,
                         "WASI is configured but no WASM engine is linked");
#else
        capabilities.set("wasi", ImplementationClass::Unsupported,
                         "WASI is off by default");
#endif
        return capabilities;
    }

    std::shared_ptr<WasmModule> load_module(
        const std::filesystem::path& path) override {
        throw Error(ErrorCode::WasmError,
                    "WASM runtime disabled; cannot load " + path.string());
    }

    std::shared_ptr<WasmInstance> instantiate(
        const WasmModule& module, const WasmSandboxConfig& config) override {
        (void)module;
        (void)config;
        throw Error(ErrorCode::WasmError, "WASM runtime disabled");
    }
};

}  // namespace

std::unique_ptr<WasmRuntime> make_wasm_runtime() {
    return std::make_unique<DisabledWasmRuntime>();
}

}  // namespace prowsetk