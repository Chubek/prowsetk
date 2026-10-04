#include "prowsetk/plugins/opencode_marionette.hpp"
#include "prowsetk/error.hpp"
#include <fstream>
#include <iostream>
namespace marionette = prowsetk::plugins::opencode_marionette;
int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) {
        std::cerr << "Usage: ptk-opencode-marionette DECISIONS.json URL OPENAPI.yaml POSTMAN.json [HTML_FILE]\n";
        return 2;
    }
    try {
        auto decisions = marionette::load_decisions(argv[1]);
        prowsetk::BrowserConfig config;
        config.observe_network = true;
        prowsetk::Browser browser(config);
        auto session = browser.create_session();
        if (argc == 6) {
            std::ifstream input(argv[5], std::ios::binary);
            if (!input) throw prowsetk::Error(prowsetk::ErrorCode::IoError, "HTML file unreadable");
            std::string html(16u * 1024u * 1024u + 1u, '\0');
            input.read(html.data(), static_cast<std::streamsize>(html.size()));
            html.resize(static_cast<std::size_t>(input.gcount()));
            if (input.bad() || html.size() > 16u * 1024u * 1024u) throw prowsetk::Error(prowsetk::ErrorCode::ResourceLimit, "HTML limit");
            session->load_html(html, argv[2]);
        } else session->navigate(argv[2]);
        // OpenCode uses its own host transport, never the page session's auth.
        auto network = prowsetk::make_socket_network_client();
        auto bridge_config = prowsetk::plugins::opencode_bridge::config_from_environment();
        bridge_config.max_requests = decisions.max_steps + 1u;
        prowsetk::plugins::opencode_bridge::OpenCodeClient client(*network, bridge_config);
        auto result = marionette::run(*session, client, decisions);
        result.extraction.write_openapi_yaml(argv[3]);
        result.extraction.write_postman_json(argv[4]);
        std::cout << "Discovered " << result.extraction.schemas.size() << " endpoint schemas; "
                  << result.steps << " actions; " << result.reason << "; coverage incomplete\n";
        return 0;
    } catch (...) {
        std::cerr << "opencode-marionette: operation failed\n";
        return 1;
    }
}
