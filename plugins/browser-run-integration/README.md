# Cloudflare Browser Run integration

`ProwseTk::browser_run` and `ptk-browser-run` connect explicitly to Cloudflare's
hosted browser. Loading the ABI-v2 native facade is network-free. Flatworm remains
the local engine; Cloudflare's remote Chromium is not a core dependency.

## Authentication and configuration

Create a Cloudflare API token with **Browser Rendering – Edit** permission for
the account. Set `CLOUDFLARE_ACCOUNT_ID` and `CLOUDFLARE_API_TOKEN`, or supply:

```toml
[cloudflare]
account_id = "your-32-character-account-id"
# api_token = "..." # secret; prefer the environment, never commit a real token
```

Nonempty environment values override TOML. With neither API-token source,
the client loads the Cloudflare-bound [oauth-assist](../oauth-assist/README.md)
cache and refreshes an expired token. An OAuth token must actually have the
necessary account permission; obtaining an OAuth token does not establish this.
Global API-key/email authentication is not implemented.

The default endpoint is fixed at
`https://api.cloudflare.com/client/v4/accounts/{account_id}/browser-run`.
Credentials are sent only as a Bearer header, including the WebSocket upgrade;
they never enter page Session headers or URL queries. HTTP redirects are rejected.
The host `NetworkClient` owns HTTPS/WSS, verified TLS and proxy configuration.

## CLI

```sh
build/default/plugins/browser-run-integration/ptk-browser-run content \
  --config Prowse.toml --url https://example.com --output page.html

build/default/plugins/browser-run-integration/ptk-browser-run cdp \
  --config Prowse.toml --method Browser.getVersion --output version.json
```

`content` uses Quick Actions `/content`. `cdp` acquires a browser or reconnects
with `--browser-session ID`, then sends one command. `--params JSON` supplies
the command object; `--session ID` supplies a flattened CDP **attachment** ID,
which is distinct from the Cloudflare browser-session ID. Attachment IDs belong
to a connection; use the C++ API for multi-command automation. CLI disconnect
does not close the remote browser; Cloudflare keep-alive/idle policies apply.
Explicitly issue `Browser.close` when finished with an owned remote browser.

The CLI reads `Prowse.toml` when present or accepts `--config FILE`. Routine
output is value-free status. `--output` explicitly exports caller data that can
contain private page values; on POSIX it creates a new `0600` file and refuses
existing files. No API token is included in the result by this plugin.

## Persistent C++ automation and existing ProwseTk tools

```cpp
#include <prowsetk/browser_run.hpp>

auto network = prowsetk::make_socket_network_client();
auto project = prowsetk::load_project_config("Prowse.toml");
prowsetk::browser_run::Client remote(
    *network, prowsetk::browser_run::resolve_config(project, *network));
auto cdp = remote.connect();
auto target = prowsetk::browser_run::create_page(*cdp, "https://example.com");
auto attachment = prowsetk::browser_run::attach_page(*cdp, target);
// Drive Page, Runtime, DOM, Input, Network, etc. on this same connection.
// The remote Chromium's CDP implementation determines supported methods.
auto result = cdp->call("Runtime.evaluate",
    R"({"expression":"document.title","returnByValue":true})", attachment);
auto events = cdp->take_events();
```

`create_page` does not wait for page readiness. The host chooses a readiness
condition before `page_html`, which snapshots the current DOM. Alternatively,
`remote.content(url)` uses Cloudflare's Quick Actions navigation/load defaults.

Use `import_html(session, html, base_url)` to query remote content with Flatworm,
PDQL, XPath, ProwseEvent emitters or existing extractors. The receiving Browser
must have `javascript=false`. Imported content is an explicit snapshot: remote
cookies, JavaScript state, HTTP status and request bodies are not transferred.
Provide an accurate base URL if the remote page redirected. Extraction retains
the existing redaction and heuristic provenance rules.

## Core CDP machinery

`include/prowsetk/cdp.hpp` adds an outbound, synchronous `CdpClient`. The existing
`CdpServer` / `prowsetk playwright` (`prowsetk cdp`) remains the inbound Flatworm
CDP bridge for Playwright and other CDP controllers. This does not extend
Flatworm to full Chromium/Puppeteer compatibility; its existing domain subset
and placeholder screenshot behavior still apply.

The client sends CDP envelopes (not JSON-RPC envelopes), correlates integer IDs
and flattened session IDs, and retains interleaved notifications. Unknown remote
methods and remote errors fail generically; peer errors are never logged.
One command is outstanding at a time, reentry is rejected, and protocol/transport
failures close the client. A caller serializes all access, including event drains.
It has no background event pump: events are read while awaiting a command reply.

Bounds: 1 MiB per CDP message/params, 1,024 queued notifications / 4 MiB total,
1,024 received messages per command, strict JSON depth 64 / 16,384 values.
The socket transport uses RFC 6455 masked client text frames, fragmentation,
ping/pong, UTF-8 validation and strict upgrade validation. Compression, binary
messages and subprotocol negotiation are not implemented. A send starts a
deadline shared by subsequent reads so notifications cannot renew the timeout.
The default is 30 seconds; DNS resolution retains the existing OS resolver behavior.
Browser keep-alive defaults to 60 seconds (allowed range 10–1,200 seconds).

All remote browsing occurs in Cloudflare: local Session request hooks do **not**
intercept Chromium's page requests. Hosts must configure remote network/action
policy through Cloudflare/CDP if required. This plugin does not turn a local
OpenCode marionette into a remote controller automatically; C++ controllers use
the explicit CDP API. There is no new Lua module or WASM contract.

Builds without OpenSSL retain the injectable core CDP and plugin APIs; the default
socket WebSocket transport reports Unsupported. Tests use memory transports and
a loopback RFC 6455 peer, never a live Cloudflare account.

References: [Browser Run CDP](https://developers.cloudflare.com/browser-run/cdp/),
[content endpoint](https://developers.cloudflare.com/browser-run/quick-actions/content-endpoint/).
