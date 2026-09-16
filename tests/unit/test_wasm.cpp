#include <gtest/gtest.h>

#include "prowsetk/error.hpp"
#include "prowsetk/wasm_runtime.hpp"

TEST(WasmRuntime, DisabledRuntimeReportsCapabilitiesHonestly) {
    const auto runtime = prowsetk::make_wasm_runtime();
    EXPECT_FALSE(runtime->enabled());
    EXPECT_FALSE(runtime->capabilities().has("wasm"));
    EXPECT_FALSE(runtime->capabilities().has("wasm.component-model"));
    EXPECT_FALSE(runtime->capabilities().has("wasi"));
}

TEST(WasmRuntime, DisabledRuntimeRefusesLoad) {
    const auto runtime = prowsetk::make_wasm_runtime();
    EXPECT_THROW(runtime->load_module("/nonexistent/module.wasm"),
                 prowsetk::Error);
}

TEST(WasmRuntime, DisabledRuntimeRefusesInstantiate) {
    // instantiate never runs because load_module rejects first; assert the
    // contract that a bogus module also throws rather than crashing.
    const auto runtime = prowsetk::make_wasm_runtime();
    struct BogusModule : prowsetk::WasmModule {
        std::string name() const override { return "bogus"; }
    } module;
    prowsetk::WasmSandboxConfig config;
    EXPECT_THROW(runtime->instantiate(module, config), prowsetk::Error);
}