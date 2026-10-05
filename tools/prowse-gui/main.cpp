#include <prowsetk/plugins/complex_gui.hpp>
#include <prowsetk/error.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
    try {
        std::string url,file,base="https://offline.test/";
        bool images=false;
        prowsetk::BrowserConfig config; config.timeout_ms=10000; config.max_response_bytes=16u*1024u*1024u;
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--help" || arg=="-h") {
                std::cout << "Usage: prowse-gui [--url HTTP(S)-URL | --file HTML-FILE]\n"
                    "  --base-url URL --no-javascript --images --proxy URL\n"
                    "Flatworm display-list browser: navigation, scrolling, links and form editing.\n"
                    "File startup uses an offline transport. Images are opt-in.\n";
                return 0;
            }
            if (arg=="--no-javascript") config.javascript=false;
            else if (arg=="--images") images=true;
            else if ((arg=="--url" || arg=="--file" || arg=="--base-url" || arg=="--proxy") && i+1<argc) {
                std::string value=argv[++i];
                if (arg=="--url") url=std::move(value);
                else if (arg=="--file") file=std::move(value);
                else if (arg=="--base-url") base=std::move(value);
                else config.proxy=prowsetk::parse_proxy_url(value);
            } else throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument,"invalid arguments");
        }
        if (!url.empty() && !file.empty()) throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument,"choose URL or file");
        std::string html;
        if (!file.empty()) {
            const auto size=std::filesystem::file_size(file);
            if (size>16u*1024u*1024u) throw prowsetk::Error(prowsetk::ErrorCode::ResourceLimit,"HTML limit");
            std::ifstream input(file,std::ios::binary); html.resize(static_cast<std::size_t>(size));
            if (!input.read(html.data(),static_cast<std::streamsize>(size))) throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument,"file read failed");
        }
        prowsetk::Browser browser(config);
        if (!file.empty()) browser.set_network_client(std::make_unique<prowsetk::MemoryNetworkClient>());
        auto session=browser.create_session();
        prowsetk::complex_gui::Viewer viewer(*session);
        viewer.controller().enable_images(images);
        if (!file.empty()) viewer.controller().load_html(html,base);
        else if (!url.empty()) viewer.controller().navigate(url);
        else viewer.controller().load_html("<body><h1>ProwseTk</h1><p>Enter an HTTP(S) address or open an HTML file.</p></body>");
        return viewer.exec();
    } catch (const prowsetk::Error& error) { std::cerr << "prowse-gui: " << prowsetk::to_string(error.code()) << '\n'; }
    catch (...) { std::cerr << "prowse-gui: operation failed\n"; }
    return 1;
}
