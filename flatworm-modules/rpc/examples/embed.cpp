#include <iostream>
#include <memory>

#include "prowsetk/browser.hpp"
#include "prowsetk/error.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: flatworm_rpc_example <rpc-module-library>\n";
        return 2;
    }
    try {
        prowsetk::Browser browser;
        browser.modules().load_native(argv[1]);
        auto transport = std::make_unique<prowsetk::MemoryNetworkClient>();
        prowsetk::HttpResponse response;
        response.status = 200;
        response.headers.emplace_back("Content-Type", "application/json");
        response.body = R"({"jsonrpc":"2.0","id":1,"result":42})";
        transport->set_response("https://example.test/rpc", std::move(response));
        browser.set_network_client(std::move(transport));
        auto session = browser.create_session();
        session->load_html(R"html(
            <p id="answer"></p>
            <script type="module">
                import {request, parseResponse} from 'flatworm:rpc';
                const message = request('add', [20, 22]);
                const http = await fetch('/rpc', {
                    method: 'POST', headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify(message)
                });
                if (!http.ok) throw new Error('RPC transport failed');
                const response = parseResponse(await http.text(), message.id);
                if (response.error) throw new Error('RPC call failed');
                document.getElementById('answer').textContent = response.result;
            </script>
        )html", "https://example.test/");
        const auto answer = session->document()->query_selector("#answer")->text();
        if (answer != "42") return 1;
        std::cout << answer << '\n';
    } catch (const prowsetk::Error& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
