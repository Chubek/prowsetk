#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "serialize_resources.hpp"
#include "prowsetk/browser.hpp"
#include "prowsetk/ir.hpp"
#include "prowsetk/network_client.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#endif

namespace {

std::string read_file(const std::string& path) {
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string quote_shell(const std::string& value) {
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
    }
    quoted += "'";
    return quoted;
}

int run_command(const std::string& command) {
#if defined(__unix__) || defined(__APPLE__)
    const int status = std::system(command.c_str());
    if (status == -1) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
#else
    (void)command;
    return -1;
#endif
}

TEST(CliCrawler, BookingExampleRunsOfflineThroughProwseConfig) {
#ifndef PROWSETK_HAVE_LUA
    GTEST_SKIP() << "Lua unavailable";
#endif
#ifndef PROWSETK_HAVE_TOMLPLUSPLUS
    GTEST_SKIP() << "TOML unavailable";
#endif
    const auto output = std::string(TEST_BINARY_DIR) + "/cli_booking_crawler.jsonl";
    const auto config = std::string(PROWSETK_SOURCE_DIR) + "/tools/crawler/booking-dotcom-admin/Prowse.toml";
    EXPECT_EQ(run_command(quote_shell(PROWSETK_CLI_BIN) + " run booking-dotcom-admin --config " +
        quote_shell(config) + " --html " + quote_shell("<h1>CLI booking crawler</h1>") + " --output " + quote_shell(output)), 0);
    EXPECT_NE(read_file(output).find("CLI booking crawler"), std::string::npos);
}

TEST(CliDriverRun, QutebrowserBookingOfflineProwseProject) {
#if !defined(QUTE_IPC_PATH) || !defined(PROWSETK_HAVE_TOMLPLUSPLUS)
    GTEST_SKIP() << "Qutebrowser project dependencies unavailable";
#else
    const auto output = std::string(TEST_BINARY_DIR) + "/qute-cli.yaml";
    const auto postman = std::string(TEST_BINARY_DIR) + "/qute-cli.postman.json";
    const auto config = std::string(PROWSETK_SOURCE_DIR) + "/examples/booking-dotcom-admin-api/Prowse.toml";
    const auto command = quote_shell(PROWSETK_CLI_BIN) + " run booking-admin-api --config " + quote_shell(config) +
        " --html " + quote_shell("<script>fetch('/api/cli?limit=3&token=private_marker')</script>") +
        " --output " + quote_shell(output) + " --postman " + quote_shell(postman) +
        " --ipc_module " + quote_shell(QUTE_IPC_PATH);
    ASSERT_EQ(run_command(command), 0);
    EXPECT_NE(read_file(output).find("/api/cli"), std::string::npos);
    EXPECT_EQ(read_file(output).find("private_marker"), std::string::npos);
    EXPECT_NE(read_file(postman).find("x-prowsetk-qutebrowser"), std::string::npos);
#endif
}

// Writes a Prowse.toml that declares the shipped drivers with absolute script
// paths, so the CLI test exercises the real driver scripts.
void write_fixture_config(const std::string& path) {
    std::ofstream stream(path);
    const std::string drivers =
        std::string(PROWSETK_SOURCE_DIR) + "/drivers/";
    stream
        << "[project]\n"
        << "name = \"cli-driver-test\"\n"
        << "root = \".\"\n"
        << "\n"
        << "[[drivers]]\n"
        << "name = \"crawl-site\"\n"
        << "description = \"CLI crawl fixture\"\n"
        << "script = \"" << drivers << "crawl_site.lua\"\n"
        << "entrypoint = \"main\"\n"
        << "enabled = true\n"
        << "\n"
        << "[[drivers.arguments]]\n"
        << "name = \"url\"\n"
        << "type = \"string\"\n"
        << "required = true\n"
        << "\n"
        << "[[drivers.arguments]]\n"
        << "name = \"html\"\n"
        << "type = \"string\"\n"
        << "required = false\n"
        << "\n"
        << "[[drivers.arguments]]\n"
        << "name = \"depth\"\n"
        << "type = \"integer\"\n"
        << "required = false\n"
        << "default = \"0\"\n"
        << "\n"
        << "[[drivers.arguments]]\n"
        << "name = \"output\"\n"
        << "type = \"path\"\n"
        << "required = false\n"
        << "default = \"build/pages.jsonl\"\n"
        << "\n"
        << "[[drivers]]\n"
        << "name = \"login\"\n"
        << "description = \"CLI login fixture\"\n"
        << "script = \"" << drivers << "login.lua\"\n"
        << "entrypoint = \"main\"\n"
        << "enabled = true\n"
        << "\n"
        << "[[drivers.arguments]]\n"
        << "name = \"url\"\n"
        << "type = \"string\"\n"
        << "required = true\n"
        << "\n"
        << "[[drivers.arguments]]\n"
        << "name = \"username\"\n"
        << "type = \"string\"\n"
        << "required = true\n"
        << "secret = true\n"
        << "\n"
        << "[[drivers.arguments]]\n"
        << "name = \"password\"\n"
        << "type = \"string\"\n"
        << "required = true\n"
        << "secret = true\n"
        << "\n"
        << "[[drivers.arguments]]\n"
        << "name = \"html\"\n"
        << "type = \"string\"\n"
        << "required = false\n"
        << "\n"
        << "[[drivers.arguments]]\n"
        << "name = \"output\"\n"
        << "type = \"path\"\n"
        << "required = false\n"
        << "default = \"build/login.json\"\n";
}

}  // namespace

TEST(CliDriverRun, RunsCrawlDriverOffline) {
#if !defined(PROWSETK_HAVE_LUA) || !defined(PROWSETK_HAVE_TOMLPLUSPLUS)
    GTEST_SKIP() << "CLI driver support requires Lua and tomlplusplus";
#else
    const std::filesystem::path fixture =
        std::filesystem::path(TEST_BINARY_DIR) / "cli_fixture";
    std::filesystem::create_directories(fixture);
    const std::string config_path = (fixture / "Prowse.toml").string();
    write_fixture_config(config_path);

    const std::string output = (fixture / "pages.jsonl").string();
    const std::string command =
        std::string(PROWSETK_CLI_BIN) +
        " run crawl-site --config " + quote_shell(config_path) +
        " --url https://example.com/ --depth 0 --output " +
        quote_shell(output) +
        " --html " +
        quote_shell("<html><head><title>CLI</title></head><body>"
                    "<a href=\"/x\">X</a></body></html>");

    EXPECT_EQ(run_command(command), 0);

    const std::string content = read_file(output);
    EXPECT_NE(content.find("\"title\":\"CLI\""), std::string::npos);
    EXPECT_NE(content.find("\"links\":[\"/x\"]"), std::string::npos);
#endif
}

TEST(CliDriverRun, RunsLoginDriverAndRedactsSecrets) {
#if !defined(PROWSETK_HAVE_LUA) || !defined(PROWSETK_HAVE_TOMLPLUSPLUS)
    GTEST_SKIP() << "CLI driver support requires Lua and tomlplusplus";
#else
    const std::filesystem::path fixture =
        std::filesystem::path(TEST_BINARY_DIR) / "cli_fixture";
    std::filesystem::create_directories(fixture);
    const std::string config_path = (fixture / "Prowse.toml").string();
    write_fixture_config(config_path);

    const std::string output = (fixture / "login.json").string();
    const std::string command =
        std::string(PROWSETK_CLI_BIN) +
        " run login --config " + quote_shell(config_path) +
        " --url https://example.com/login --username alice --password "
        "supersecret --output " +
        quote_shell(output) + " --html " +
        quote_shell("<html><body><form action=\"/auth\" method=\"post\">"
                    "<input name=\"email\">"
                    "<input name=\"password\" type=\"password\">"
                    "</form></body></html>");

    EXPECT_EQ(run_command(command), 0);

    const std::string content = read_file(output);
    EXPECT_NE(content.find("\"submitted\":false"), std::string::npos);
    EXPECT_NE(content.find("\"credentials_redacted\":true"),
              std::string::npos);
    EXPECT_EQ(content.find("supersecret"), std::string::npos);
    EXPECT_EQ(content.find("alice"), std::string::npos);
#endif
}

TEST(CliDriverRun, RejectsUnknownDriverAndArguments) {
#if !defined(PROWSETK_HAVE_LUA) || !defined(PROWSETK_HAVE_TOMLPLUSPLUS)
    GTEST_SKIP() << "CLI driver support requires Lua and tomlplusplus";
#else
    const std::filesystem::path fixture =
        std::filesystem::path(TEST_BINARY_DIR) / "cli_fixture";
    std::filesystem::create_directories(fixture);
    const std::string config_path = (fixture / "Prowse.toml").string();
    write_fixture_config(config_path);

    const std::string binary = PROWSETK_CLI_BIN;
    EXPECT_NE(run_command(binary + " run nope --config " +
                          quote_shell(config_path)),
              0);
    EXPECT_NE(run_command(binary + " run crawl-site --config " +
                          quote_shell(config_path) + " --url x --bogus y"),
              0);
    EXPECT_NE(run_command(binary + " run crawl-site --config " +
                          quote_shell(config_path)),
              0);
#endif
}

TEST(CliSerialize, WritesEventsImlAndVtdToStandardOutput) {
    const std::filesystem::path fixture =
        std::filesystem::path(TEST_BINARY_DIR) / "cli_serialize";
    std::filesystem::create_directories(fixture);
    const auto iml = fixture / "page.iml";
    const auto events = fixture / "page.events.ndjson";
    const auto vtd = fixture / "page.vtd";
    const std::string html =
        "<html><body><h1>Serialized</h1><p>page</p>"
        "<script>document.querySelector(':scope')</script></body></html>";
    const std::string binary = quote_shell(PROWSETK_CLI_BIN);

    const std::string iml_command =
        binary + " serialize --iml --stdout --html " + quote_shell(html) +
        " > " + quote_shell(iml.string());
    ASSERT_EQ(run_command(iml_command), 0);
    EXPECT_NE(read_file(iml.string()).find("(document"), std::string::npos);

    const std::string events_command =
        binary + " serialize --events --stdout --html " + quote_shell(html) +
        " > " + quote_shell(events.string());
    ASSERT_EQ(run_command(events_command), 0);
    const std::string event_lines = read_file(events.string());
    EXPECT_NE(event_lines.find("\"kind\":\"start\""), std::string::npos);
    EXPECT_NE(event_lines.find("\"attributes\":"), std::string::npos);

    const std::string vtd_command =
        binary + " serialize --vtd --stdout --html " + quote_shell(html) +
        " > " + quote_shell(vtd.string());
    ASSERT_EQ(run_command(vtd_command), 0);
    const std::string vtd_bytes = read_file(vtd.string());
    ASSERT_GE(vtd_bytes.size(), 5U);
    EXPECT_EQ(vtd_bytes.substr(0, 5), "PVTD1");

    EXPECT_NE(run_command(binary + " serialize --stdout --html " +
                          quote_shell(html)), 0);
    EXPECT_NE(run_command(binary + " serialize --iml --vtd --stdout --html " +
                          quote_shell(html)), 0);
    EXPECT_NE(run_command(binary + " serialize --events --vtd --stdout --html " +
                          quote_shell(html)), 0);
}

TEST(CliSerialize, SnapshotsSameOriginStylesAndImages) {
    prowsetk::BrowserConfig config;
    config.javascript = false;
    prowsetk::Browser browser(config);
    auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
    auto* requests = network.get();
    prowsetk::HttpResponse page;
    page.status = 200;
    page.headers.emplace_back("Content-Type", "text/html");
    page.body = "<html><head><link rel='stylesheet' href='/main.css'></head>"
                "<body><img src='/logo.png'><img src='https://other.test/private.png'>"
                "</body></html>";
    network->set_response("https://example.test/", page);
    prowsetk::HttpResponse css;
    css.status = 200;
    css.headers.emplace_back("Content-Type", "text/css");
    css.body = "body{background-color:#abcdef}";
    network->set_response("https://example.test/main.css", css);
    prowsetk::HttpResponse image;
    image.status = 200;
    image.headers.emplace_back("Content-Type", "image/png");
    image.body = "png-bytes";
    network->set_response("https://example.test/logo.png", image);
    browser.set_network_client(std::move(network));

    auto session = browser.create_session();
    session->navigate("https://example.test/");
    auto document = session->document();
    ASSERT_NE(document, nullptr);
    prowsetk::cli::embed_serialized_resources(*session, *document);

    const std::string iml = prowsetk::emit_prowse_iml(*document);
    EXPECT_NE(iml.find("background-color:#abcdef"), std::string::npos);
    EXPECT_NE(iml.find("data:image/png;base64,cG5nLWJ5dGVz"), std::string::npos);
    EXPECT_NE(iml.find("https://other.test/private.png"), std::string::npos);
    for (const auto& request : requests->requests()) {
        EXPECT_EQ(request.url.find("https://other.test/"), std::string::npos);
    }
}

TEST(CliDriverRun, BookingDotcomOfflineOpenApi) {
#if !defined(PROWSETK_HAVE_LUA) || !defined(PROWSETK_HAVE_TOMLPLUSPLUS)
    GTEST_SKIP() << "CLI driver support requires Lua and tomlplusplus";
#else
    const std::string output = std::string(TEST_BINARY_DIR) + "/booking-cli.yaml";
    const std::string postman_output = output + ".postman.json";
    const std::string log = output + ".log";
    const std::string command = quote_shell(PROWSETK_CLI_BIN) +
        " run booking-dotcom-admin --config " +
        quote_shell(std::string(PROWSETK_SOURCE_DIR) + "/examples/booking-dotcom-admin-scrape/Prowse.toml") +
        " --html " + quote_shell("<a href='/reservations'>Reservations</a><script>fetch('/api/hotels')</script>") +
        " --output " + quote_shell(output) +
        " --postman " + quote_shell(postman_output) +
        " > " + quote_shell(log) + " 2>&1";
    ASSERT_EQ(run_command(command), 0) << read_file(log);
    const auto yaml = read_file(output);
    EXPECT_NE(yaml.find("openapi: 3.1.0"), std::string::npos);
    // api-only is on by default: the plain page is gunk, the API call stays.
    EXPECT_EQ(yaml.find("'/reservations':"), std::string::npos);
    EXPECT_NE(yaml.find("/api/hotels"), std::string::npos);
    EXPECT_NE(yaml.find("authenticated: false"), std::string::npos);
#endif
}

TEST(CliDriverRun, BookingDotcomFlashOnOfflineStaysDeterministic) {
#if !defined(PROWSETK_HAVE_LUA) || !defined(PROWSETK_HAVE_TOMLPLUSPLUS)
    GTEST_SKIP() << "CLI driver support requires Lua and tomlplusplus";
#else
    const std::string output = std::string(TEST_BINARY_DIR) + "/booking-cli-flash-on.yaml";
    const std::string postman_output = output + ".postman.json";
    const std::string log = output + ".log";
    const std::string command = quote_shell(PROWSETK_CLI_BIN) +
        " run booking-dotcom-admin --config " +
        quote_shell(std::string(PROWSETK_SOURCE_DIR) + "/examples/booking-dotcom-admin-scrape/Prowse.toml") +
        " --html " + quote_shell("<a href='/reservations'>Reservations</a><script>fetch('/api/hotels')</script>") +
        " --flash-on true" +
        " --output " + quote_shell(output) +
        " --postman " + quote_shell(postman_output) +
        " > " + quote_shell(log) + " 2>&1";
    ASSERT_EQ(run_command(command), 0) << read_file(log);
    const auto yaml = read_file(output);
    EXPECT_NE(yaml.find("/api/hotels"), std::string::npos);
    EXPECT_NE(yaml.find("used: false"), std::string::npos);
#endif
}
