// Demonstrates offline document parsing and CSS selection. No network access is
// required, so the example is deterministic and safe to run anywhere.

#include <iostream>

#include <prowsetk/document.hpp>

int main() {
    const char* html = R"HTML(
        <html>
          <head><title>ProwseTk Example</title></head>
          <body>
            <h1 id="title">Links</h1>
            <a href="https://example.com/one">One</a>
            <a href="https://example.com/two">Two</a>
          </body>
        </html>)HTML";

    auto document = prowsetk::parse_html(html, "https://example.com/");

    std::cout << "title: " << document->title() << '\n';
    for (const auto& link : document->query_selector_all("a")) {
        std::cout << "link: " << link->attribute("href") << " -> "
                  << link->text() << '\n';
    }
    return 0;
}
