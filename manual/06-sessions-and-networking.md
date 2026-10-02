# Chapter 06: Sessions and Networking

[Manual index](README.md)

## Navigation and requests

`session->navigate(url)` fetches a document, follows configured redirects,
parses HTML, installs the DOM, and executes supported page scripts.
`session->load_html(html, base_url)` installs already available markup.
`session->request(request)` returns a response without automatically parsing its
body into the session document.

```cpp
prowsetk::HttpRequest request;
request.method = "GET";
request.url = "https://example.com/";
request.timeout_ms = 10000;
request.max_response_bytes = 1024 * 1024;
const auto response = session->request(request);
if (response.ok()) {
    session->load_html(response.body, response.final_url);
}
```

`ok()` means HTTP 2xx. A response also has headers, body, final URL, and redirect
chain. A completed transport operation can return an HTTP error status;
transport/parse failures use exceptions. Inspect the response before making an
authentication or successful-page claim.

## Defaults and redirects

Core `BrowserConfig` defaults include JavaScript enabled, redirects enabled,
10 redirect hops, a 30,000-ms request timeout, and a 32-MiB response bound.
The HTML parser's separate 16-MiB input limit still applies. Specialized tools
set tighter limits, documented in their chapters.

For POST redirects, 301/302/303 convert the request to GET; 307/308 retain method
and body. Valid `Set-Cookie` headers enter the session's scoped jar before the
next redirect request. Default session headers are set with `set_header` and
cleared with `clear_headers`; raw credentials are application-owned inputs.

All page-script requests, dynamic script loads, and synthetic-action requests
use the owning session. Enforce origin/request policy at the transport boundary
when every redirect hop and page subrequest must obey it. The standalone
crawler, pagewatch, and spider hosts supply such guards. A general Browser is
not automatically a same-origin crawler merely because a declarative project
security table exists.

## HTTPS and proxy transport

When OpenSSL 3 is available, the POSIX transport verifies certificate chains
and hostnames, sends SNI, and requires TLS 1.2 or newer. OpenSSL's default CA
paths include `SSL_CERT_FILE` and `SSL_CERT_DIR`. There is no insecure
certificate-verification override. Without OpenSSL, HTTPS reports unsupported
functionality and HTTP remains available.

HTTP, HTTPS, and SOCKS5 proxy URLs use `http://host:port`, `https://host:port`,
and `socks5://[user:pass@]host:port`. Prefer the explicit host configuration:

```cpp
config.proxy = prowsetk::parse_proxy_url("http://127.0.0.1:3128");
```

A request proxy overrides the browser proxy. When neither is set, the socket
transport checks the scheme's proxy environment variables and then
`ALL_PROXY`/`all_proxy`. Proxy diagnostics redact credentials. CLI navigation,
endpoint extraction, serialization, and driver hosts accept `--proxy URL`.

## Hermetic transports

```cpp
auto network = std::make_unique<prowsetk::MemoryNetworkClient>();
prowsetk::HttpResponse page;
page.status = 200;
page.body = "<h1>Fixture</h1>";
network->set_response("https://example.test/", page);
browser.set_network_client(std::move(network));
auto session = browser.create_session();
session->navigate("https://example.test/");
```

MemoryNetworkClient records requests and can use a handler for dynamic response
logic. This keeps login, redirect, and script tests independent of public
network services. A custom NetworkClient implements `send(const HttpRequest&)`;
ownership transfers to Browser through a unique pointer.

## Session lifetime

Replacing a document replaces the active page host and page-JS handles. A
rejected bounded HTML parse preserves the previous document and URL. Browser
headers, cookies, JavaScript state, and tool process state have different
lifetime rules; use the storage and daemon chapters when persistence matters.

## Concurrency

The current public `navigate`, `request`, and script calls are synchronous.
Embedders schedule them through their own task/thread infrastructure and
serialize access to a session's mutable state. There is no public
`navigate_async` or `request_async` method in the current headers. Crawler runs
one session; pagewatch and spider isolate independent browser work in supervised
processes. The service gateway queues work above the synchronous core API.

Reference: `browser.hpp`, `network_client.hpp`, `url.hpp`, socket transport.

**Next:** [DOM, selectors, and XPath](07-dom-selectors-and-xpath.md).
