#include <prowsetk/plugins/basic_gui.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <prowsetk/error.hpp>

int main(int argc, char** argv) {
    try {
        std::string url, file, marionette, goal, base = "https://offline.test/";
        prowsetk::BrowserConfig config;
        config.timeout_ms = 10000;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: ptk-basic-gui [--url HTTP(S)-URL | --file HTML-FILE]\n"
                             "                     [--base-url URL] [--no-javascript] [--proxy URL]\n"
                             "                     [--marionette LUA-FILE] [--goal TEXT]\n"
                             "Flatworm inspector: basic page preview, DOM, sanitized source, console and network.\n";
                return 0;
            }
            if (arg == "--no-javascript") config.javascript = false;
            else if ((arg == "--url" || arg == "--file" || arg == "--base-url" || arg == "--proxy" || arg == "--marionette" || arg == "--goal") && i + 1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--url") url = value;
                else if (arg == "--file") file = value;
                else if (arg == "--base-url") base = value;
                else if (arg == "--marionette") marionette = value;
                else if (arg == "--goal") goal = value;
                else config.proxy = prowsetk::parse_proxy_url(value);
            } else throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "invalid arguments");
        }
        if (!url.empty() && !file.empty()) throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "choose URL or file");
        if (!goal.empty() && marionette.empty())
            throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "goal requires marionette");
        prowsetk::Browser browser(config);
        // Offline startup rejects every network request, including script and
        // synthetic navigation. FLTK never fetches page resources itself.
        if (!file.empty()) browser.set_network_client(std::make_unique<prowsetk::MemoryNetworkClient>());
        auto session = browser.create_session();
        if (!prowsetk::basic_gui::available()) {
            throw prowsetk::Error(prowsetk::ErrorCode::Unsupported,
                                  "built without FLTK");
        }
        prowsetk::basic_gui::Viewer viewer(*session);
        if (!url.empty()) viewer.navigate(url);
        if (!file.empty()) {
            const auto size = std::filesystem::file_size(file);
            if (size > 16u * 1024u * 1024u) throw prowsetk::Error(prowsetk::ErrorCode::ResourceLimit, "HTML file limit");
            std::ifstream stream(file, std::ios::binary);
            std::string html(static_cast<std::size_t>(size), '\0');
            if (!stream.read(html.data(), static_cast<std::streamsize>(html.size()))) throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "HTML file read failed");
            viewer.load_html(html, base);
        }
        if (!marionette.empty()) {
            viewer.show();
            viewer.run_marionette_file(marionette, goal);
        }
        return viewer.exec();
    } catch (const prowsetk::Error& error) {
        std::cerr << "basic-gui: " << prowsetk::to_string(error.code()) << '\n';
    } catch (...) { std::cerr << "basic-gui: operation failed\n"; }
    return 1;
}
