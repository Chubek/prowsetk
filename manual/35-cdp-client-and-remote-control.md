# Chapter 35: CDP Client and Remote Control

[Manual index](README.md)

## Two directions of browser control

ProwseTk has both an inbound Chrome DevTools Protocol (CDP) server and an outbound
CDP client. Choose the interface according to which browser executes the page:

| Interface | Controller | Page engine | Entry point |
|---|---|---|---|
| Inbound CDP bridge | External Playwright or another CDP client | Local Flatworm | `prowsetk cdp` / `prowsetk playwright`, `CdpServer` |
| Outbound CDP client | A ProwseTk C++ host | The remote browser | `CdpClient` in `prowsetk/cdp.hpp` |
| Cloudflare integration | A ProwseTk host or `ptk-browser-run` | Cloudflare's hosted browser | `browser_run::Client`, Chapter 36 |

[Chapter 28](28-web-and-python-interfaces.md) covers inbound discovery at
`/json/version` and `/json/list`, and Playwright's `connect_over_cdp`. That server
maps its supported domains to Flatworm; it retains restricted browser semantics
and placeholder screenshots. Adding the outbound client does not expand the
server into a full Chromium implementation or guarantee every Puppeteer workflow.

The outbound client supplies CDP command transport for host-written marionettes
and other controllers. The remote peer determines which domain methods work.
There is no automatic remote backend for the existing Lua or OpenCode controllers.

## Connecting from C++

Link the core library, and include both the CDP and networking interfaces:

```cmake
find_package(ProwseTk CONFIG REQUIRED)
add_executable(remote_control main.cpp)
target_link_libraries(remote_control PRIVATE ProwseTk::core)
```

The following is a complete connection example. Pass an actual browser WebSocket
endpoint obtained from the server's discovery response; an HTTP discovery URL
itself is not a WebSocket endpoint.

```cpp
#include <prowsetk/cdp.hpp>
#include <prowsetk/error.hpp>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        auto network = prowsetk::make_socket_network_client();
        prowsetk::HttpRequest upgrade;
        upgrade.url = argv[1]; // ws:// or wss://, without embedded credentials
        upgrade.timeout_ms = 30000;
        upgrade.max_response_bytes = 1024 * 1024;
        prowsetk::CdpClient client(network->open_websocket(upgrade));
        const auto version = client.call("Browser.getVersion");
        // Parse version with the host's JSON library before consuming fields.
        // Results and events are caller data, not automatically redacted logs.
        (void)version;
        std::cout << "CDP command completed\n";
        return 0;
    } catch (const prowsetk::Error&) {
        std::cerr << "CDP operation failed\n";
        return 1;
    }
}
```

`NetworkClient::open_websocket(HttpRequest)` is the host transport boundary.
The default implementation on the base class reports Unsupported; the socket
client supplies WS/WSS when built with OpenSSL. Hosts can replace either the
network client or the message transport without changing CDP commands.

The socket implementation verifies WSS certificates and hostnames. It uses the
existing proxy selection, including `HTTPS_PROXY`, `HTTP_PROXY` and `NO_PROXY`
after mapping WSS/WS to HTTPS/HTTP. See [Chapter 6](06-sessions-and-networking.md).
Upgrade headers are explicit; page cookies and Session headers are not attached
automatically, and WebSocket redirects are not followed.

## Command and event API

| Operation | Contract |
|---|---|
| `CdpClient(std::unique_ptr<WebSocket>)` | Takes ownership of a non-null transport |
| `call(method, params = "{}", session_id = {})` | Sends one command and returns its result object as JSON |
| `take_events()` | Returns queued notification envelopes and clears the queue |
| `WebSocket::send(text)` | Sends a text message; custom transports enforce their limits |
| `WebSocket::receive()` | Returns one complete text message, including reassembly if needed |

CDP uses envelopes such as:

```json
{"id":1,"method":"Runtime.evaluate","params":{"expression":"document.title","returnByValue":true},"sessionId":"attachment-id"}
```

The client generates the numeric command ID and verifies that the reply matches
both that ID and the requested flattened `sessionId`. `params` must be a JSON
object. A successful `call` returns only the reply's `result` object, not the
outer envelope. A `Runtime.evaluate` result can itself contain `result` and
`exceptionDetails`; hosts must inspect these before treating page evaluation as
successful. This is CDP, not a JSON-RPC 2.0 envelope with a `jsonrpc` field.

Notifications arriving before the reply are retained as complete JSON envelopes.
They can concern other targets or sessions on the connection. Dispatch them by
their `method` and `sessionId` after draining the queue. There is no background
reader or subscription callback: idle notifications are read when a command is
awaiting its reply.

## Target attachment and controller lifetime

A typical remote-browser controller performs these operations on one connection:

1. Create or select a target using `Target.createTarget` / `Target.getTargets`.
2. Attach with `Target.attachToTarget` and `flatten: true`.
3. Parse the returned attachment `sessionId`.
4. Pass that attachment ID to page-level `Page`, `Runtime`, `DOM`, `Input`, or
   `Network` commands supported by the peer.
5. Inspect command results and page postconditions, draining notifications
   between operations so the queue stays bounded.
6. Close targets or the owned remote browser explicitly when finished.

For example, after obtaining an attachment ID:

```cpp
auto enabled = client.call("Page.enable", "{}", attachment_id);
auto ready = client.call("Runtime.evaluate",
    R"({"expression":"document.readyState","returnByValue":true})",
    attachment_id);
```

This checks current state; it is not a navigation wait helper. The controller
chooses a finite readiness/action budget and parses the result. Chapter 36
provides target/attachment helpers and a remote HTML snapshot workflow.

Calls are synchronous. Serialize access to a client, including `take_events`;
only one command may be outstanding. Recursive command entry is rejected.
Malformed or mismatched responses, peer command errors, transport failures and
response-budget exhaustion invalidate the connection. Establish a new client
after such a failure; the client does not retry potentially state-changing
commands. Invalid local arguments rejected before sending do not by themselves
close the connection.

Destroying the client releases its transport. This does not issue `Browser.close`
or establish that the remote browser has been destroyed. Use the remote service's
lifecycle policy or an explicit command for that operation.

## Limits and transport support

| Resource | Implemented bound |
|---|---|
| Method name / attachment ID | 256 bytes each |
| Params / received CDP message | 1 MiB each; encoded envelopes also face the JSON codec's bound |
| Notifications retained | 1,024 entries and 4 MiB aggregate |
| Messages examined per command | 1,024 |
| JSON structure | 64 levels, 16,384 values; duplicate keys and invalid UTF-8 rejected |
| Socket upgrade headers received | 64 KiB |
| Frames examined by one receive | 4,096 |
| Socket timeout setting | 1–300,000 ms; `HttpRequest` defaults to 30,000 ms |

The socket transport masks client frames, validates the HTTP upgrade, reassembles
fragmented text, handles ping/pong and checks UTF-8. Binary messages, compression
and subprotocol negotiation are unsupported. A send starts the transaction
deadline shared by subsequent reads, so notifications do not renew the timeout.
DNS retains the existing operating-system resolver behavior. Custom transports
must supply their own finite time and message bounds.

Remote responses and events may contain page data. The API returns them to the
host; errors use generic messages rather than echoing peer error text. Local
Session request hooks do not automatically govern another browser's page traffic.

## Verification and sources

```sh
ctest --preset default -R 'cdp-client|remote-protocols'
ctest --preset asan -R 'cdp-client|remote-protocols'
```

These cases use injected messages and a loopback WebSocket peer. They exercise
correlation, queue limits, reentry, masking, fragmentation, ping/pong, malformed
handshakes, oversized frames and invalid UTF-8 without a cloud account.

Reference: [public CDP API](../include/prowsetk/cdp.hpp),
[network API](../include/prowsetk/network_client.hpp), `src/core/cdp.cpp`,
`src/core/network_client.cpp`, and the separate inbound `src/core/playwright.cpp`.

**Next:** [Cloudflare Browser Run](36-cloudflare-browser-run.md).
