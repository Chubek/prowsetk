// ProwseTk command-line interface.
//
// Commands:
//   prowsetk version                          print the ProwseTk version
//   prowsetk serve [--host H] [--port P] ...  run the web interface
//   prowsetk webdriver [--host H] [--port P]  run the builtin W3C WebDriver
//   prowsetk endpoints --url URL ...          extract endpoints to OpenAPI YAML
//   prowsetk run <driver> [--arg value ...]   run a Prowse.toml Lua driver
//
// The CLI is a thin host application over the C++ core: it creates a Browser,
// drives sessions, and (for `serve`) exposes the embedded web interface. All
// network access remains host-mediated through the engine's NetworkClient.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <prowsetk/browser.hpp>
#include <prowsetk/endpoint_extraction.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/lua_runtime.hpp>
#include <prowsetk/project_config.hpp>
#include <prowsetk/version.hpp>
#include <prowsetk/web_interface.hpp>

namespace {

struct Arguments {
    std::string command;
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string web_root;
    std::string url;
    std::string output;
    bool javascript = true;
    std::string driver;
    std::string config_path = "Prowse.toml";
    std::string user_agent;
    std::vector<std::pair<std::string, std::string>> run_args;
};

std::string default_web_root(const char* executable) {
    std::error_code error;
#if defined(__linux__)
    auto binary = std::filesystem::canonical("/proc/self/exe", error);
    if (error) {
        binary = std::filesystem::canonical(executable, error);
    }
#else
    const auto binary = std::filesystem::canonical(executable, error);
#endif
    if (!error) {
        const auto installed = binary.parent_path().parent_path() /
            "share/prowsetk/web";
        if (std::filesystem::is_regular_file(installed / "index.html", error)) {
            return installed.string();
        }
    }
#ifdef PROWSETK_WEB_ROOT
    return PROWSETK_WEB_ROOT;
#else
    return "resources/web";
#endif
}

bool parse_arguments(int argc, char** argv, Arguments& args) {
    if (argc < 2) {
        return false;
    }
    args.command = argv[1];
    args.web_root = default_web_root(argv[0]);

    if (args.command == "run") {
        if (argc < 3) {
            return false;
        }
        args.driver = argv[2];
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--config" && i + 1 < argc) {
                args.config_path = argv[++i];
            } else if (arg == "--user-agent" && i + 1 < argc) {
                args.user_agent = argv[++i];
            } else if (arg.rfind("--", 0) == 0 && i + 1 < argc) {
                args.run_args.emplace_back(arg.substr(2), argv[++i]);
            } else {
                std::cerr << "prowsetk run: unexpected argument: " << arg
                          << '\n';
                return false;
            }
        }
        return true;
    }

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--host" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            args.port = std::stoi(argv[++i]);
        } else if (arg == "--web-root" && i + 1 < argc) {
            args.web_root = argv[++i];
        } else if (arg == "--url" && i + 1 < argc) {
            args.url = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            args.output = argv[++i];
        } else if (arg == "--javascript") {
            args.javascript = true;
        } else if (arg == "--no-javascript") {
            args.javascript = false;
        } else {
            std::cerr << "prowsetk: unknown argument: " << arg << '\n';
            return false;
        }
    }
    return true;
}

void print_usage(std::ostream& out) {
    out << "usage:\n"
        << "  prowsetk version\n"
        << "  prowsetk serve [--host 127.0.0.1] [--port 8080] [--web-root DIR] "
           "[--no-javascript]\n"
        << "  prowsetk webdriver [--host 127.0.0.1] [--port 0] [--no-javascript]\n"
        << "  prowsetk cdp [--host 127.0.0.1] [--port 0] [--no-javascript]\n"
        << "  prowsetk endpoints --url URL [--output FILE] [--javascript]\n"
        << "  prowsetk run <driver> [--arg VALUE ...] [--config Prowse.toml] "
           "[--user-agent VALUE]\n";
}

int run_serve(const Arguments& args) {
    prowsetk::WebInterfaceConfig config;
    config.browser.javascript = args.javascript;
    config.web_root = args.web_root;

    prowsetk::WebInterface interface(std::move(config));
    prowsetk::HttpServer server(interface, args.host,
                                static_cast<std::uint16_t>(args.port));

    std::cout << "ProwseTk " << prowsetk::version()
              << " web interface listening on http://" << args.host << ":"
              << server.port() << "/\n";
    std::cout << "web root: " << interface.web_root() << "\n";
    std::cout.flush();

    server.run();
    return 0;
}

int run_webdriver(const Arguments& args) {
    prowsetk::WebInterfaceConfig config;
    config.browser.javascript = args.javascript;
    // WebDriver is a protocol endpoint, not a static UI. Keeping web_root
    // empty also makes accidental exposure of project files impossible.
    config.web_root.clear();
    prowsetk::WebInterface interface(std::move(config));
    prowsetk::HttpServer server(interface, args.host,
                                static_cast<std::uint16_t>(args.port));
    std::cout << "ProwseTk " << prowsetk::version()
              << " WebDriver listening on http://" << args.host << ":"
              << server.port() << "\n";
    std::cout.flush();
    server.run();
    return 0;
}

int run_playwright(const Arguments& args) {
    prowsetk::WebInterfaceConfig config;
    config.browser.javascript = args.javascript;
    config.enable_playwright = true;
    // CDP is a protocol endpoint, not a static UI.
    config.web_root.clear();
    prowsetk::WebInterface interface(std::move(config));
    prowsetk::HttpServer server(interface, args.host,
                                static_cast<std::uint16_t>(args.port));
    std::cout << "ProwseTk " << prowsetk::version()
              << " Playwright/CDP listening on http://" << args.host << ":"
              << server.port() << "\n";
    std::cout.flush();
    server.run();
    return 0;
}
int run_endpoints(const Arguments& args) {
    if (args.url.empty()) {
        std::cerr << "prowsetk endpoints: missing --url\n";
        return 2;
    }

    prowsetk::BrowserConfig config;
    config.javascript = args.javascript;

    prowsetk::Browser browser(std::move(config));
    auto session = browser.create_session();
    session->navigate(args.url);

    prowsetk::EndpointExtractor extractor;
    const auto result = extractor.extract(*session->document());

    for (const auto& warning : result.warnings) {
        std::cerr << "warning: " << warning << '\n';
    }

    if (!args.output.empty()) {
        std::ofstream output(args.output, std::ios::binary);
        if (!output) {
            std::cerr << "prowsetk endpoints: cannot open output file: "
                      << args.output << '\n';
            return 1;
        }
        output << result.openapi_yaml;
        std::cout << "wrote " << result.endpoints.size() << " endpoints to "
                  << args.output << '\n';
    } else {
        std::cout << result.openapi_yaml;
    }
    return 0;
}

int run_driver(const Arguments& args) {
    prowsetk::ProjectConfig config;
    try {
        config = prowsetk::load_project_config(args.config_path);
    } catch (const prowsetk::Error& error) {
        std::cerr << "prowsetk run: " << error.what() << '\n';
        return 2;
    }

    const auto it = std::find_if(
        config.drivers.begin(), config.drivers.end(),
        [&](const prowsetk::DriverConfig& driver) {
            return driver.name == args.driver;
        });
    if (it == config.drivers.end()) {
        std::cerr << "prowsetk run: unknown driver: " << args.driver << '\n';
        return 2;
    }
    const prowsetk::DriverConfig& driver = *it;
    if (!driver.enabled) {
        std::cerr << "prowsetk run: driver is disabled: " << args.driver
                  << '\n';
        return 2;
    }
    if (driver.script.empty()) {
        std::cerr << "prowsetk run: driver has no script: " << args.driver
                  << '\n';
        return 2;
    }

    // Resolve the script relative to the Prowse.toml's project root.
    std::filesystem::path config_dir =
        std::filesystem::path(args.config_path).parent_path();
    if (config_dir.empty()) {
        config_dir = ".";
    }
    const std::filesystem::path project_root =
        std::filesystem::path(config.root).is_absolute()
            ? std::filesystem::path(config.root)
            : config_dir / config.root;
    const std::filesystem::path script_path = project_root / driver.script;

    // Map command-line `--name value` pairs onto the driver's declared
    // arguments, applying defaults and rejecting unknown or missing ones.
    std::map<std::string, std::string> provided;
    for (const auto& [name, value] : args.run_args) {
        provided[name] = value;
    }
    std::vector<prowsetk::LuaArgument> lua_args;
    for (const auto& declared : driver.arguments) {
        std::string type = declared.type.empty() ? "string" : declared.type;
        const auto found = provided.find(declared.name);
        if (found != provided.end()) {
            lua_args.push_back(
                prowsetk::LuaArgument{declared.name, type, found->second});
            provided.erase(found);
        } else if (!declared.default_value.empty()) {
            lua_args.push_back(prowsetk::LuaArgument{
                declared.name, type, declared.default_value});
        } else if (declared.required) {
            std::cerr << "prowsetk run: missing required argument: --"
                      << declared.name << '\n';
            return 2;
        }
    }
    for (const auto& [name, value] : provided) {
        (void)value;
        std::cerr << "prowsetk run: unknown argument: --" << name << '\n';
        return 2;
    }

    prowsetk::BrowserConfig browser_config;
    browser_config.javascript = config.javascript;
    browser_config.user_agent =
        config.user_agent.empty() ? browser_config.user_agent : config.user_agent;
    browser_config.user_agent =
        args.user_agent.empty() ? browser_config.user_agent : args.user_agent;
    browser_config.follow_redirects = config.follow_redirects;
    browser_config.max_redirects = config.max_redirects;
    browser_config.timeout_ms = config.timeout_ms;
    browser_config.observe_network = config.observe_network;
    browser_config.unsupported_api_behavior = config.unsupported_api_behavior;

    prowsetk::Browser browser(std::move(browser_config));
    try {
        for (const auto& plugin : config.plugins) {
            if (!plugin.enabled || !plugin.autoload || plugin.path.empty()) {
                continue;
            }
            std::filesystem::path plugin_path = plugin.path;
            if (plugin_path.is_relative()) {
                plugin_path = project_root / plugin_path;
            }
            const std::string type = plugin.type.empty() ? "native" : plugin.type;
            if (type == "native") {
                browser.plugins().load_native(plugin_path);
            } else if (type == "lua") {
                browser.plugins().load_lua(plugin_path);
            } else if (type == "wasm") {
                browser.plugins().load_wasm(plugin_path, prowsetk::WasmSandboxConfig{});
            } else {
                std::cerr << "prowsetk run: unsupported plugin type for "
                          << plugin.name << ": " << type << '\n';
                return 2;
            }
        }
        browser.plugins().initialize_all();
    } catch (const prowsetk::Error& error) {
        std::cerr << "prowsetk run: " << error.what() << '\n';
        return 1;
    }

    prowsetk::LuaRuntime lua;
    lua.bind_browser(&browser);

    const prowsetk::LuaResult loaded = lua.run_file(script_path.string());
    if (!loaded.ok) {
        std::cerr << "prowsetk run: " << lua.last_error() << '\n';
        return 1;
    }
    std::string exit_value;
    const prowsetk::LuaResult called =
        lua.call_function(driver.entrypoint, lua_args, &exit_value);
    if (!called.ok) {
        std::cerr << "prowsetk run: " << lua.last_error() << '\n';
        return 1;
    }
    int code = 0;
    if (!exit_value.empty()) {
        try {
            code = std::stoi(exit_value);
        } catch (const std::exception&) {
            code = 0;
        }
    }
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    Arguments args;
    if (!parse_arguments(argc, argv, args)) {
        print_usage(std::cerr);
        return 2;
    }

    try {
        if (args.command == "version") {
            std::cout << prowsetk::version() << '\n';
            return 0;
        }
        if (args.command == "serve") {
            return run_serve(args);
        }
        if (args.command == "webdriver") {
            return run_webdriver(args);
        }
        if (args.command == "playwright" || args.command == "cdp") {
            return run_playwright(args);
        }
        if (args.command == "endpoints") {
            return run_endpoints(args);
        }
        if (args.command == "run") {
            return run_driver(args);
        }
        std::cerr << "prowsetk: unknown command: " << args.command << '\n';
        print_usage(std::cerr);
        return 2;
    } catch (const prowsetk::Error& error) {
        std::cerr << "prowsetk: error: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "prowsetk: error: " << error.what() << '\n';
        return 1;
    }
}
