#include <dlfcn.h>
#include <gtest/gtest.h>

#include <string>

#include "prowsetk/browser.hpp"
#include "prowsetk/plugins/beacon.hpp"

namespace beacon = prowsetk::plugins::beacon;

namespace {

beacon::FlashRequest request_fixture(
    std::string id = "11111111-1111-4111-8111-111111111111") {
    beacon::FlashFilters filters;
    filters.url_pattern = "https://example.com/*";
    filters.resource_types = {"main_frame", "script"};
    return beacon::FlashRequest{id, beacon::RequestType::PageDom, filters};
}

}  // namespace

TEST(BeaconRequestType, RoundTripsKnownNames) {
    EXPECT_EQ(beacon::parse_request_type("page_dom"),
              beacon::RequestType::PageDom);
    EXPECT_EQ(beacon::parse_request_type("network_info"),
              beacon::RequestType::NetworkInfo);
    EXPECT_EQ(beacon::parse_request_type("stylesheet"),
              beacon::RequestType::Stylesheet);
    EXPECT_EQ(beacon::parse_request_type("tracepoint"),
              beacon::RequestType::Tracepoint);
    EXPECT_EQ(std::string(beacon::to_string(beacon::RequestType::NetworkInfo)),
              "network_info");
    EXPECT_FALSE(beacon::parse_request_type("nonsense").has_value());
}

TEST(BeaconResourceType, AcceptsDocumentedSet) {
    EXPECT_TRUE(beacon::is_valid_resource_type("main_frame"));
    EXPECT_TRUE(beacon::is_valid_resource_type("stylesheet"));
    EXPECT_TRUE(beacon::is_valid_resource_type("script"));
    EXPECT_FALSE(beacon::is_valid_resource_type("websocket-evil"));
}

TEST(BeaconValidation, RejectsEmptyPatternAndUnknownResource) {
    beacon::FlashRequest bad = request_fixture();
    bad.filters.url_pattern.clear();
    EXPECT_FALSE(beacon::validate_flash_request(bad, nullptr));
    bad = request_fixture();
    bad.filters.resource_types = {"nope"};
    std::string error;
    EXPECT_FALSE(beacon::validate_flash_request(bad, &error));
    EXPECT_FALSE(error.empty());
}

TEST(BeaconValidation, RejectsMalformedId) {
    auto bad = request_fixture("not-a-uuid!!");
    EXPECT_FALSE(beacon::validate_flash_request(bad, nullptr));
    bad = request_fixture("11111111");
    EXPECT_FALSE(beacon::validate_flash_request(bad, nullptr));
}

TEST(BeaconRender, FlashRequestIsDeterministicJson) {
    const std::string json = beacon::render_flash_request(request_fixture());
    EXPECT_NE(json.find("\"type\":\"flash_request\""), std::string::npos);
    EXPECT_NE(json.find("11111111-1111-4111-8111-111111111111"),
              std::string::npos);
    EXPECT_NE(json.find("\"request_type\":\"page_dom\""), std::string::npos);
    EXPECT_NE(json.find("https://example.com/*"), std::string::npos);
}

TEST(BeaconRender, RedactsSensitiveQueryValues) {
    auto req = request_fixture();
    req.filters.url_pattern = "https://example.com/?api_key=supersecret";
    const std::string json = beacon::render_flash_request(req);
    EXPECT_EQ(json.find("supersecret"), std::string::npos);
    EXPECT_NE(json.find("[REDACTED]"), std::string::npos);
}

TEST(BeaconRender, FlashListCarriesStatus) {
    beacon::FlashDescriptor descriptor;
    descriptor.flash_id = "11111111-1111-4111-8111-111111111111";
    descriptor.request_type = beacon::RequestType::Stylesheet;
    descriptor.filters.url_pattern = "https://example.com/*";
    descriptor.status = beacon::FlashStatus::Seeking;
    const std::string json = beacon::render_flash_list({descriptor});
    EXPECT_NE(json.find("\"type\":\"flash_list\""), std::string::npos);
    EXPECT_NE(json.find("\"status\":\"seeking\""), std::string::npos);
}

TEST(BeaconRender, ConnectDataAndTracepoint) {
    beacon::FlashConnect connect{"11111111-1111-4111-8111-111111111111", 42};
    const std::string connect_json = beacon::render_flash_connect(connect);
    EXPECT_NE(connect_json.find("\"tab_id\":42"), std::string::npos);

    beacon::FlashData data;
    data.flash_id = connect.flash_id;
    data.data_type = "page_dom";
    data.html = "<html></html>";
    data.url = "https://example.com/page";
    data.timestamp = 1727155200;
    const std::string data_json = beacon::render_flash_data(data);
    EXPECT_NE(data_json.find("\"type\":\"flash_data\""), std::string::npos);
    EXPECT_NE(data_json.find("1727155200"), std::string::npos);

    beacon::TracepointEvent event;
    event.flash_id = connect.flash_id;
    event.event = beacon::TracepointEventKind::DomMutation;
    event.selector = "#element";
    event.detail = "child-added";
    const std::string event_json = beacon::render_tracepoint_event(event);
    EXPECT_NE(event_json.find("\"event\":\"dom_mutation\""),
              std::string::npos);
}

TEST(BeaconParse, RoundTripsRequestAndConnect) {
    const std::string request_json =
        beacon::render_flash_request(request_fixture());
    auto parsed = beacon::parse_flash_request(request_json);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->flash_id, "11111111-1111-4111-8111-111111111111");
    EXPECT_EQ(parsed->request_type, beacon::RequestType::PageDom);
    EXPECT_EQ(parsed->filters.resource_types,
              (std::vector<std::string>{"main_frame", "script"}));

    beacon::FlashConnect connect{"11111111-1111-4111-8111-111111111111", 7};
    auto parsed_connect =
        beacon::parse_flash_connect(beacon::render_flash_connect(connect));
    ASSERT_TRUE(parsed_connect.has_value());
    EXPECT_EQ(parsed_connect->tab_id, 7);
    EXPECT_FALSE(
        beacon::parse_flash_request("{\"type\":\"nope\"}").has_value());
}

TEST(BeaconParse, DoesNotTreatNestedPayloadAsProtocol) {
    EXPECT_FALSE(beacon::parse_message_type(
        "{\"payload\":{\"type\":\"flash_connect\"}}").has_value());
    EXPECT_FALSE(beacon::parse_flash_id(
        "{\"payload\":{\"flash_id\":\"private\"}}").has_value());
    const auto invalid = beacon::parse_flash_request(
        "{\"type\":\"flash_request\",\"flash_id\":\"11111111-1111-4111-8111-111111111111\","
        "\"request_type\":\"page_dom\",\"filters\":{\"url_pattern\":\"ok\","
        "\"resource_types\":[\"forged\"]}}");
    ASSERT_TRUE(invalid.has_value());
    EXPECT_FALSE(beacon::validate_flash_request(*invalid));
    const auto scoped = beacon::parse_flash_request(
        "{\"type\":\"flash_request\",\"flash_id\":\"11111111-1111-4111-8111-111111111111\","
        "\"request_type\":\"page_dom\",\"decoy\":{\"url_pattern\":\"https://wrong.test/*\"},"
        "\"filters\":{\"url_pattern\":\"https://right.test/*\"}}");
    ASSERT_TRUE(scoped.has_value());
    EXPECT_EQ(scoped->filters.url_pattern, "https://right.test/*");
}

TEST(BeaconNativeFraming, EncodeDecodeRoundTrip) {
    const std::string json = beacon::render_ping();
    const auto framed = beacon::encode_native_message(json);
    ASSERT_EQ(framed.size(), json.size() + 4u);
    // Little-endian length prefix.
    EXPECT_EQ(framed[0], static_cast<std::uint8_t>(json.size()));
    std::string decoded;
    ASSERT_TRUE(beacon::decode_native_message(framed, &decoded, nullptr));
    EXPECT_EQ(decoded, json);
}

TEST(BeaconNativeFraming, RejectsTruncatedAndOversize) {
    std::string error;
    EXPECT_FALSE(beacon::decode_native_message(
        std::vector<std::uint8_t>{0x01}, nullptr, &error));
    EXPECT_FALSE(error.empty());
    const std::string big(16, 'x');
    // Hand-crafted oversize prefix (0xFFFFFFFF) must be rejected.
    EXPECT_FALSE(beacon::decode_native_message(
        std::vector<std::uint8_t>{0xFF, 0xFF, 0xFF, 0xFF}, nullptr, nullptr));
    EXPECT_TRUE(beacon::encode_native_message(
                    std::string(beacon::kMaxNativeMessageBytes + 1, 'y'))
                    .empty());
    (void)big;
}

TEST(BeaconManager, FullFlashLifecycle) {
    beacon::FlashSessionManager manager;
    ASSERT_TRUE(manager.create(request_fixture(), 1000, nullptr));
    EXPECT_EQ(manager.size(), 1u);
    ASSERT_EQ(manager.list_seeking().size(), 1u);

    std::string error;
    EXPECT_FALSE(manager.create(request_fixture(), 1000, &error));
    EXPECT_FALSE(error.empty());  // duplicate id

    EXPECT_TRUE(manager.connect("11111111-1111-4111-8111-111111111111", 42,
                                1500, nullptr));
    auto found =
        manager.find("11111111-1111-4111-8111-111111111111");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->status, beacon::FlashStatus::Connected);
    EXPECT_EQ(manager.connected_tab("11111111-1111-4111-8111-111111111111"),
              std::optional<int>(42));
    EXPECT_TRUE(manager.list_seeking().empty());
    EXPECT_FALSE(manager.connect("11111111-1111-4111-8111-111111111111", 43,
                                 1600, &error));
    EXPECT_EQ(manager.connected_tab("11111111-1111-4111-8111-111111111111"),
              std::optional<int>(42));

    EXPECT_TRUE(manager.disconnect("11111111-1111-4111-8111-111111111111",
                                   nullptr));
    EXPECT_EQ(manager.size(), 0u);
    EXPECT_FALSE(manager.disconnect("missing-id", nullptr));
}

TEST(BeaconManager, DeliversOnlyForConnectedTabAndBoundsQueue) {
    beacon::FlashSessionManager manager;
    const auto id = request_fixture().flash_id;
    ASSERT_TRUE(manager.create(request_fixture(), 0));
    std::string error;
    EXPECT_FALSE(manager.publish(id, 42, "private", 1, &error));
    ASSERT_TRUE(manager.connect(id, 42, 2));
    EXPECT_FALSE(manager.publish(id, 43, "private", 3, &error));
    EXPECT_FALSE(manager.poll(id, 4).has_value());
    for (int i = 0; i < 16; ++i) {
        ASSERT_TRUE(manager.publish(id, 42, std::to_string(i), 5));
    }
    EXPECT_FALSE(manager.publish(id, 42, "overflow", 5, &error));
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(manager.poll(id, 6), std::to_string(i));
    }
    EXPECT_FALSE(manager.poll(id, 7).has_value());
    EXPECT_FALSE(manager.publish(id, 42,
        std::string(beacon::kMaxNativeMessageBytes + 1, 'x'), 8, &error));
    ASSERT_TRUE(manager.disconnect(id));
    EXPECT_FALSE(manager.publish(id, 42, "stale", 9, &error));
}

TEST(BeaconParse, RejectsOutOfRangeTabIdsAndTrailingFrameBytes) {
    EXPECT_FALSE(beacon::parse_flash_connect(
        "{\"type\":\"flash_connect\",\"flash_id\":\"x\",\"tab_id\":2147483648}")
        .has_value());
    EXPECT_FALSE(beacon::parse_tab_id("{\"tab_id\":-1}").has_value());
    const auto frame = beacon::encode_native_message(beacon::render_ping());
    auto extra = frame;
    extra.push_back('x');
    EXPECT_FALSE(beacon::decode_native_message(extra, nullptr));
}

TEST(BeaconManager, ExpiresIdleFlashes) {
    beacon::BeaconOptions options;
    options.timeout_ms = 100;
    beacon::FlashSessionManager manager(options);
    ASSERT_TRUE(manager.create(request_fixture(), 0, nullptr));
    EXPECT_EQ(manager.expire_stale(50), 0u);
    EXPECT_EQ(manager.expire_stale(1000), 1u);
    EXPECT_EQ(manager.size(), 0u);
}

TEST(BeaconManager, EnforcesCapacityAndValidation) {
    beacon::BeaconOptions options;
    options.max_flashes = 1;
    beacon::FlashSessionManager manager(options);
    ASSERT_TRUE(manager.create(request_fixture(), 0, nullptr));
    beacon::FlashFilters filters;
    filters.url_pattern = "https://example.com/*";
    auto second = beacon::new_flash(beacon::RequestType::NetworkInfo, filters,
                                    "22222222-2222-4222-8222-222222222222");
    std::string error;
    EXPECT_FALSE(manager.create(second, 0, &error));
    EXPECT_FALSE(error.empty());
}

TEST(BeaconManager, ListRenderingStaysHermetic) {
    beacon::FlashSessionManager manager;
    ASSERT_TRUE(manager.create(request_fixture(), 0, nullptr));
    const std::string list = manager.render_list();
    EXPECT_NE(list.find("flash_list"), std::string::npos);
}

TEST(BeaconIds, GeneratedIdsAreUuidShaped) {
    const std::string id = beacon::make_flash_id();
    EXPECT_EQ(id.size(), 36u);
    EXPECT_TRUE(beacon::validate_flash_request(
        beacon::new_flash(beacon::RequestType::PageDom,
                          beacon::FlashFilters{"https://example.com/*", {}}, id),
        nullptr));
}

TEST(BeaconPluginAbi, ExposesEntrySymbol) {
    void* handle = dlopen(BEACON_PLUGIN_PATH, RTLD_NOW);
    ASSERT_NE(handle, nullptr) << dlerror();
    using Entry = const void* (*)();
    auto entry = reinterpret_cast<Entry>(dlsym(handle, "prowsetk_plugin_entry"));
    ASSERT_NE(entry, nullptr);
    EXPECT_NE(entry(), nullptr);
    dlclose(handle);
}

TEST(BeaconPlugin, LoadsAndReportsCapabilities) {
    prowsetk::Browser browser;
    browser.plugins().load_native(BEACON_PLUGIN_PATH);
    ASSERT_EQ(browser.plugins().initialize_all(), 1u);
    EXPECT_TRUE(browser.plugins().has_capability("beacon-oracle"));
    EXPECT_TRUE(browser.plugins().has_capability("flash-lifecycle"));
}
