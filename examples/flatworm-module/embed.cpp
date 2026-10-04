#include <iostream>

#include "prowsetk/browser.hpp"
#include "prowsetk/error.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: prowsetk_example_flatworm_modules <native-module-library>\n";
        return 2;
    }
    try {
        prowsetk::Browser browser;
        browser.modules().load_native(argv[1]);
        auto session = browser.create_session();
        session->load_html(R"html(
            <p id="answer"></p>
            <script type="module">
                import {add} from 'flatworm:math';
                document.getElementById('answer').textContent = add(20, 22);
            </script>
        )html", "https://example.test/");
        std::cout << session->document()->query_selector("#answer")->text() << '\n';
    } catch (const prowsetk::Error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
