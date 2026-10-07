#include <gtest/gtest.h>
#include "prowsetk/gfx_backend.hpp"
#include "prowsetk/error.hpp"
#include <stdexcept>

extern "C" {
const ProwseGfxBackend* test_gfx_backend(void);
int test_gfx_destroyed(void);
int test_gfx_submitted(void);
}
using namespace prowsetk;

TEST(GfxBackend, HeadlessIsDisplayFreeAndRejectsMalformedFrames) {
    GfxBackend backend("headless");
    backend.submit(encode_gfx_frame({{}, {{0, 0, "Preview"}}}));
    backend.run();
    EXPECT_THROW(backend.submit({}), Error);
    EXPECT_THROW(backend.submit(encode_gfx_frame({{640, 480}, {}})), Error);
    EXPECT_THROW(GfxBackend("unknown"), Error);
    EXPECT_THROW(GfxBackend("bgfx"), Error);
    EXPECT_THROW(GfxBackend("headless", {0, 1}), Error);
}

TEST(GfxBackend, CDefinitionOwnershipAndValidationBeforeDispatch) {
    const auto* definition = test_gfx_backend();
    {
        GfxBackend backend(*definition);
        backend.submit(encode_gfx_frame({{}, {}}));
        EXPECT_EQ(test_gfx_submitted(), 1);
        EXPECT_THROW(backend.submit({}), Error);
        EXPECT_EQ(test_gfx_submitted(), 1);
        backend.run();
        EXPECT_EQ(test_gfx_destroyed(), 0);
    }
    EXPECT_EQ(test_gfx_destroyed(), 1);
}

TEST(GfxBackend, RejectsAbiAndContainsNativeExceptions) {
    auto definition = *test_gfx_backend();
    ++definition.abi_version;
    EXPECT_THROW(GfxBackend backend(definition), Error);
    definition = *test_gfx_backend(); definition.struct_size = 0;
    EXPECT_THROW(GfxBackend backend(definition), Error);
    definition = *test_gfx_backend(); definition.submit = nullptr;
    EXPECT_THROW(GfxBackend backend(definition), Error);
    definition = *test_gfx_backend(); definition.name = "";
    EXPECT_THROW(GfxBackend backend(definition), Error);
    definition = *test_gfx_backend();
    definition.run = [](void*) -> ProwseGfxStatus { throw std::runtime_error("SECRET_CALLBACK"); };
    GfxBackend backend(definition);
    try { backend.run(); FAIL(); }
    catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::PluginError);
        EXPECT_EQ(std::string(error.what()).find("SECRET"), std::string::npos);
    }
}

TEST(GfxBackend, FailedCreationCleansTransferredState) {
    auto definition = *test_gfx_backend();
    static ProwseGfxStatus (*original)(const ProwseGfxSurface*, void**) = nullptr;
    original = definition.create;
    definition.create = [](const ProwseGfxSurface* surface, void** instance) {
        original(surface, instance);
        return PROWSETK_GFX_ERROR;
    };
    EXPECT_THROW(GfxBackend backend(definition), Error);
    EXPECT_EQ(test_gfx_destroyed(), 1);
}

TEST(GfxBackend, ContainsErrorsAndCleansStateAfterThrowingCreation) {
    auto definition = *test_gfx_backend();
    static ProwseGfxStatus (*original)(const ProwseGfxSurface*, void**) = nullptr;
    original = definition.create;
    definition.create = [](const ProwseGfxSurface* surface, void** instance) -> ProwseGfxStatus {
        original(surface, instance);
        throw Error(ErrorCode::IoError, "SECRET_CREATE");
    };
    EXPECT_THROW(GfxBackend backend(definition), Error);
    EXPECT_EQ(test_gfx_destroyed(), 1);

    definition = *test_gfx_backend();
    definition.run = [](void*) -> ProwseGfxStatus { throw Error(ErrorCode::IoError, "SECRET_RUN"); };
    GfxBackend backend(definition);
    try { backend.run(); FAIL(); }
    catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::PluginError);
        EXPECT_EQ(std::string(error.what()).find("SECRET"), std::string::npos);
    }
}
