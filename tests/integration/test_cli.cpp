#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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