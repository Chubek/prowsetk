#include "prowsetk/embedding.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/gfx_backend.hpp"
#include <charconv>
#include <fstream>
#include <iostream>
#include <string>

namespace {
[[noreturn]] void arguments() {
    throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "invalid arguments");
}
std::uint32_t dimension(std::string_view value) {
    std::uint32_t number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !number || number > prowsetk::max_gfx_dimension) arguments();
    return number;
}
std::string read_html(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw prowsetk::Error(prowsetk::ErrorCode::IoError, "input unavailable");
    constexpr std::size_t limit = 16u * 1024u * 1024u;
    std::string html;
    char block[8192];
    while (file.read(block, sizeof(block)) || file.gcount()) {
        const auto count = static_cast<std::size_t>(file.gcount());
        if (count > limit - html.size())
            throw prowsetk::Error(prowsetk::ErrorCode::ResourceLimit, "HTML input too large");
        html.append(block, count);
    }
    if (file.bad()) throw prowsetk::Error(prowsetk::ErrorCode::IoError, "input read failed");
    return html;
}
}

int main(int argc, char** argv) {
    try {
        std::string backend(prowsetk::default_gfx_backend());
        std::string file, url;
        prowsetk::GfxOptions surface;
        bool check = false, dump = false, javascript = false, list = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view option(argv[i]);
            const auto value = [&]() -> std::string_view {
                if (++i == argc) arguments();
                return argv[i];
            };
            if (option == "--help" || option == "-h") {
                std::cout << "prowse-gui: event-stream GFX preview\n"
                    "Usage: prowse-gui [-T BACKEND] (--file HTML | --url URL) [OPTIONS]\n"
                    "  -T, --backend NAME   Select GFX backend\n"
                    "  --list-backends      Show compiled backend availability\n"
                    "  --width N --height N Surface dimensions (1..16384)\n"
                    "  --check              Submit the frame without opening a window\n"
                    "  --dump-text          Print sanitized preview text explicitly\n"
                    "  --javascript         Enable page JavaScript (off by default)\n";
                return 0;
            } else if (option == "-T" || option == "--backend") backend = value();
            else if (option == "--file") { if (!file.empty()) arguments(); file = value(); }
            else if (option == "--url") { if (!url.empty()) arguments(); url = value(); }
            else if (option == "--width") surface.width = dimension(value());
            else if (option == "--height") surface.height = dimension(value());
            else if (option == "--check") check = true;
            else if (option == "--dump-text") dump = true;
            else if (option == "--javascript") javascript = true;
            else if (option == "--list-backends") list = true;
            else arguments();
        }
        if (list) {
            if (!file.empty() || !url.empty()) arguments();
            for (const auto& info : prowsetk::gfx_backends())
                std::cout << info.name << '\t' << (info.available ? "available" : "unavailable") << '\n';
            return 0;
        }
        if (file.empty() == url.empty() || backend.empty()) arguments();
        prowsetk::GfxBackend graphics(backend, surface);
        prowsetk::BrowserConfig config;
        config.javascript = javascript;
        prowsetk::EmbeddedBrowser browser(config);
        if (!file.empty()) browser.load_html(read_html(file));
        else browser.navigate(url);
        prowsetk::ProwseEventStream events;
        browser.stream_events([&](const prowsetk::ProwseEvent& event) {
            events.push_back(event);
            return prowsetk::EventStreamControl::Continue;
        });
        const auto bytes = prowsetk::emit_gfx_ir(events, surface);
        graphics.submit(bytes);
        if (dump) {
            for (const auto& text : prowsetk::decode_gfx_frame(bytes).text)
                std::cout << text.text << '\n';
        }
        if (!check) graphics.run();
        return 0;
    } catch (const prowsetk::Error& error) {
        std::cerr << "prowse-gui: " << prowsetk::to_string(error.code()) << '\n';
    } catch (...) { std::cerr << "prowse-gui: operation failed\n"; }
    return 1;
}
