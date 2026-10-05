# Chapter 36: Cloudflare Browser Run

[Manual index](README.md)

## Facilities and build targets

`plugins/browser-run-integration` supplies an explicit client for Cloudflare's
hosted browser service:

- `browser_run::Client::content` fetches rendered HTML through Quick Actions.
- `browser_run::Client::connect` acquires or reconnects to a browser through CDP.
- C++ helpers create targets, attach to pages, capture HTML and import snapshots
  into Flatworm for existing query and extraction tools.
- `ptk-browser-run` exposes HTML fetching and single-command CDP operations.

The local engine remains Flatworm. Cloudflare executes remote page scripts and
network requests; the plugin does not embed Chromium into ProwseTk.

```sh
cmake --preset default
cmake --build --preset default --target ptk-browser-run
build/default/plugins/browser-run-integration/ptk-browser-run --help
```

Installed executables can be invoked as `ptk-browser-run`. C++ applications link
`ProwseTk::browser_run` and include `prowsetk/browser_run.hpp`. The native ABI-v2
facade is `libprowsetk_browser_run_plugin.so` on Linux; loading it performs no
connection or authentication. Actual operations are explicit CLI/C++ calls.
Live HTTPS/WSS through the socket client requires the optional OpenSSL build.

## Account authentication

For direct API-token authentication, create a Cloudflare token with **Browser
Rendering – Edit** permission on the selected account. Supply credentials in
the environment or in `Prowse.toml`:

```toml
[cloudflare]
account_id = "your-32-character-hex-account-id"
# api_token = "..." # secret-bearing alternative to CLOUDFLARE_API_TOKEN
```

The account placeholder must be replaced with a real 32-character hexadecimal
account ID. The supported credential sources are:

| Input | Environment override | Resolution |
|---|---|---|
| `[cloudflare].account_id` | `CLOUDFLARE_ACCOUNT_ID` | Nonempty environment value, then TOML |
| `[cloudflare].api_token` | `CLOUDFLARE_API_TOKEN` | Nonempty environment value, then TOML, then OAuth cache |
| `[oauth].client_id` | `PROWSETK_OAUTH_CLIENT_ID` | Used for OAuth cache identity and refresh |
| `[oauth].scopes` | `PROWSETK_OAUTH_SCOPES` | Used for OAuth cache identity and refresh |

When no API token is supplied, `resolve_config` uses the Cloudflare-bound cache
from [Chapter 37](37-oauth-assistance.md). It refreshes an expired token and saves
the replacement. A valid cached token avoids an additional refresh request.
OAuth requires provider-approved client/scopes and does not itself prove Browser
Run permission. Global API-key/email authentication is not implemented.

The API origin is fixed at `https://api.cloudflare.com`, with the account path
`/client/v4/accounts/{account_id}/browser-run`. Credentials are Bearer headers on
control requests and WebSocket upgrades, never URL query parameters or local
page Session headers. Redirected HTTP responses are rejected.

## Fetch rendered HTML from the CLI

With account credentials supplied, run:

```sh
build/default/plugins/browser-run-integration/ptk-browser-run content \
  --config Prowse.toml \
  --url https://example.com/ \
  --output build/remote-page.html
```

This sends a POST to Quick Actions `/content` with a `url` field, checks the
Cloudflare success envelope and writes its HTML result. The wrapper uses the
service's navigation/load defaults; it does not expose arbitrary Quick Actions
options, selector waits, PDF or screenshot endpoints.

The CLI loads `Prowse.toml` from the working directory when it exists, or the
file selected by `--config`. `content` requires both `--url` and `--output`.
The output parent directory must exist. On POSIX, exports are newly created
`0600` files; existing destinations are refused. The HTML is explicit caller
data, not a sanitized endpoint export. Routine terminal output is a value-free
completion or failure message.

## Send a CDP command

```sh
build/default/plugins/browser-run-integration/ptk-browser-run cdp \
  --config Prowse.toml \
  --method Browser.getVersion \
  --output build/remote-version.json
```

| CLI option | Meaning |
|---|---|
| `--method DOMAIN.METHOD` | Required CDP method |
| `--params JSON` | Params object; defaults to `{}` |
| `--browser-session ID` | Reconnect to a known Cloudflare browser session; otherwise acquire one |
| `--session ID` | Flattened CDP attachment ID for a page-level command |
| `--output FILE` | Optional explicit export of the command's result object |
| `--config FILE` | Project configuration override |

Without `--output`, the result is consumed but not printed. Each CLI invocation
opens one connection and sends one command. A CDP attachment ID belongs to its
connection and is not a persistent credential for the next invocation. Use a
persistent C++ client for target attachment followed by page commands.

## Distinguish the identifiers

| Identifier | Owner and use |
|---|---|
| Cloudflare browser-session ID | Service-managed browser; passed to `connect(id)` or `--browser-session` |
| CDP target ID | A page/target inside that browser; passed to `attach_page` |
| CDP attachment `sessionId` | Returned by attaching on a connection; passed to `CdpClient::call` and `page_html` |
| Local Flatworm Session | A separate C++ object created by `Browser::create_session`; receives imported HTML |

Neither disconnecting the CLI nor destroying a `CdpClient` issues `Browser.close`.
Cloudflare keep-alive and idle policies apply. A host should explicitly close an
owned remote browser when its workflow is finished, or deliberately retain it
according to the service's session lifecycle.

## Persistent remote controller

This C++ fragment keeps all commands on one connection:

```cpp
#include <prowsetk/browser_run.hpp>

auto network = prowsetk::make_socket_network_client();
auto project = prowsetk::load_project_config("Prowse.toml");
auto settings = prowsetk::browser_run::resolve_config(project, *network);
prowsetk::browser_run::Client remote(*network, settings);
auto cdp = remote.connect();
auto target = prowsetk::browser_run::create_page(*cdp, "https://example.com/");
auto attachment = prowsetk::browser_run::attach_page(*cdp, target);
auto state = cdp->call("Runtime.evaluate",
    R"({"expression":"document.readyState","returnByValue":true})", attachment);
auto events = cdp->take_events();
```

`create_page` navigates via the target-creation URL but does not wait for page
readiness. Parse `state`, inspect `exceptionDetails` where applicable, and apply
the host's bounded readiness and action policy. After satisfying that policy,
`browser_run::page_html(*cdp, attachment)` returns the current
`document.documentElement.outerHTML`. It does not supply an additional wait.

Methods in `Page`, `Runtime`, `DOM`, `Input` and `Network` can be sent through
`call`; their availability and effects belong to the remote Chromium instance.
Use [Chapter 35](35-cdp-client-and-remote-control.md) for correlation, events,
error handling and transport lifetime. Keep the `NetworkClient` alive while the
`browser_run::Client` uses its reference.

## Import into existing Flatworm workflows

The following complete example fetches HTML and queries it locally without
executing its scripts again:

```cpp
#include <prowsetk/browser_run.hpp>
#include <prowsetk/error.hpp>

int main() {
    try {
        auto network = prowsetk::make_socket_network_client();
        auto project = prowsetk::load_project_config("Prowse.toml");
        prowsetk::browser_run::Client remote(
            *network, prowsetk::browser_run::resolve_config(project, *network));
        const auto html = remote.content("https://example.com/");

        prowsetk::BrowserConfig settings;
        settings.javascript = false;
        prowsetk::Browser local(settings);
        auto snapshot = local.create_session();
        prowsetk::browser_run::import_html(
            *snapshot, html, "https://example.com/");
        auto headings = snapshot->document()->query_selector_all("h1");
        return headings.empty() ? 1 : 0;
    } catch (const prowsetk::Error&) {
        return 2;
    }
}
```

`import_html` requires a JavaScript-disabled receiving Browser and an HTTP(S)
base URL. Supply the actual page base if remote navigation redirected; the
`content` helper returns HTML only and does not separately report the final URL.
Imported snapshots can feed [DOM/XPath](07-dom-selectors-and-xpath.md),
[PDQL](08-pdql.md), [endpoint extraction](15-endpoint-discovery.md) and
[ProwseEvent emitters](22-intermediate-representations.md).

Import transfers no cookies, JavaScript runtime state, HTTP status, response
bodies for discovered endpoints or remote network history. Script references in
HTML are static evidence, not observed requests. Preserve redaction, provenance
and incomplete-coverage metadata in generated specifications. Enrichment probes
through a local Session are separate requests without remote authentication.

## Bounds, policy and troubleshooting

| Setting | Default / bound |
|---|---|
| C++ `Config::timeout_ms` | 30,000 ms; allowed 1–300,000 |
| C++ `Config::keep_alive_ms` | 60,000 ms; allowed 10,000–1,200,000 |
| `/content` response / CDP messages | 1 MiB |
| Endpoint selection | Fixed Cloudflare API origin; no CLI base-URL override |

The timeout and keep-alive settings are C++ fields, not currently TOML fields or
CLI flags. Acquiring a browser adds `keep_alive` to the connection URL;
reconnecting uses the existing browser-session path. The core CDP queue and JSON
bounds from Chapter 35 also apply.

Local Session hooks do not intercept remote Chromium's navigation, subresources
or script requests. Remote action/network policy must be configured through the
remote service or CDP. There is no new Lua module, Python binding, WASM contract,
or automatic OpenCode marionette backend supplied by this plugin.

For connection failures, check account ID, token permission and OpenSSL support.
For missing page content, establish readiness before a CDP snapshot. For output
failure, check the parent directory and select a new filename. Detailed peer
responses and credentials are not echoed by the CLI.

```sh
ctest --preset default -R 'browser-run|remote-protocols'
```

These tests exercise memory transports, snapshot import, OAuth refresh
composition and a loopback peer. They do not authenticate against Cloudflare.

Reference: [plugin guide](../plugins/browser-run-integration/README.md),
[public API](../include/prowsetk/browser_run.hpp),
[Cloudflare CDP](https://developers.cloudflare.com/browser-run/cdp/), and
[Quick Actions content](https://developers.cloudflare.com/browser-run/quick-actions/content-endpoint/).

**Next:** [OAuth assistance](37-oauth-assistance.md).
