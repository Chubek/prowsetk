#include "prowsetk/plugins/ebpf_interface.hpp"

#include <gtest/gtest.h>

using namespace prowsetk::plugins::ebpf_interface;

TEST(EbpfInterface, CollectsAndRedactsHostRequests) {
    EndpointCollector collector;
    collector.observe_request("POST", "https://example.test/api/items?token=secret&x=1");
    collector.observe_response("https://example.test/api/items?token=secret&x=1", 201,
                               "application/json");
    const auto json = collector.render_json();
    EXPECT_NE(json.find("POST"), std::string::npos);
    EXPECT_NE(json.find("[REDACTED]"), std::string::npos);
    EXPECT_NE(json.find("201"), std::string::npos);
    EXPECT_EQ(json.find("secret"), std::string::npos);
}

TEST(EbpfInterface, DisabledRuntimeReportsCapabilities) {
    Runtime runtime;
#ifndef PROWSETK_HAVE_LIBBPF
    EXPECT_FALSE(runtime.available());
    std::string error;
    EXPECT_FALSE(runtime.load_object("missing.o", &error));
    EXPECT_NE(error.find("libbpf"), std::string::npos);
#endif
}
