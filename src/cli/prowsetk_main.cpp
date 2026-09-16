// ProwseTk command-line interface.
//
// Commands:
//   prowsetk version                          print the ProwseTk version
//   prowsetk serve [--host H] [--port P] ...  run the web interface
//   prowsetk endpoints --url URL ...          extract endpoints to OpenAPI YAML
//
// The CLI is a thin host application over the C++ core: it creates a Browser,
// drives sessions, and (for `serve`) exposes the embedded web interface. All
// network access remains host-mediated through the engine's NetworkClient.

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <prowsetk/browser.hpp>
#include <prowsetk/endpoint_extraction.hpp>
#include <prowsetk/error.hpp>
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
};

std::string default_web_root() {
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
    args.web_root = default_web_root();

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
        << "  prowsetk endpoints --url URL [--output FILE] [--javascript]\n";
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
        if (args.command == "endpoints") {
            return run_endpoints(args);
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
