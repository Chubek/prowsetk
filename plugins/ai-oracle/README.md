# ai-oracle

`ai-oracle` provides synchronous OpenAI **Responses API** inquiries for ProwseTk
plugins and drivers. Scrapers can request CAPTCHA image/text assistance; spiders
can request structured crawl recommendations. Each result carries an answer,
model, response ID, token usage when supplied, `provenance = "openai-responses"`,
and `advisory = true`. The caller applies the recommendation through its existing
browser/session or plugin interface.

## Build and install

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default -R ai-oracle
```

The optional target builds when OpenAIpp and JSON headers are available. Populate
`third_party/openaipp` from [OpenAIpp](https://github.com/Chubek/OpenAIpp), including
its `cpp-httplib`, `MetaTk/DSLtk`, and `nlohmann-json` dependencies. CMake first
checks installed `openaipp` / `nlohmann_json` packages, then these vendored headers.
`-DPROWSETK_BUILD_AI_ORACLE=OFF` omits the plugin. Lua bindings additionally
require the project's Lua development dependency. WASM is not required.

Build artifacts:

- `build/<preset>/plugins/ai-oracle/libprowsetk_ai_oracle.so`: ABI-v2 native
  registration and exported C oracle service.
- `ProwseTk::ai_oracle`: static C++/C service library, exported in the installed
  ProwseTk CMake package.
- `build/<preset>/plugins/ai-oracle/ai_oracle.so`: callable Lua module.

Installation places the native plugin in `lib/prowsetk/plugins`, the Lua module
in `lib/prowsetk/lua`, and headers under `include/prowsetk/plugins`. Set
`LUA_CPATH` to the installed module directory (`/prefix/lib/prowsetk/lua/?.so;;`).
Native plugin metadata registration alone does not load a Lua module.

### OpenAIpp adapter

The implementation uses `OpenAI.hpp`'s `openapipp::low` JSON builders,
authentication headers, and environment lookup. This checkout's concrete
`HttpClient` cannot accept an injected transport, so the adapter sends the
resulting wire request through ProwseTk's **NetworkClient**. It neither
instantiates the upstream socket client nor changes vendored code. Explicit
aggregate configuration also avoids the upstream unset-environment defaults.
JSON parsing uses OpenAIpp's nested nlohmann-json dependency.

## Configuration

Configuration belongs to each oracle instance. Loading/initializing the native
plugin makes no requests. Inquiries require `enabled = true` and an API key.

| Option | Default | Accepted bounds / meaning |
|---|---|---|
| `enabled` | `false` | Enable external API inquiries |
| `api_key` | empty | Bearer key; Lua defaults to `OPENAI_API_KEY` |
| `base_url` | `https://api.openai.com/v1` | Explicit HTTPS API base without userinfo, query, or fragment; `/responses` is appended |
| `model` | `gpt-4o-mini` | Nonempty model ID, at most 256 bytes; availability depends on the API account |
| `organization` / `project` | empty | Optional OpenAI organization/project headers |
| `timeout_ms` | `30000` | 1–300000 milliseconds |
| `max_input_bytes` | `262144` | 1–2097152 bytes, including raw HTML, prompt, JSON context, page URL, and embedded images together |
| `max_response_bytes` | `1048576` | 1–4194304 bytes |
| `max_requests` | `16` | 1–1024 send attempts per instance |
| `max_output_tokens` | `512` | 16–8192 tokens |

C++ `options_from_environment()` and Lua `new` read `OPENAI_API_KEY`,
`OPENAI_ORG_ID`, and `OPENAI_PROJECT_ID`. They leave the oracle disabled and do
not read an endpoint from the environment. The C API takes explicit options;
`prowsetk_ai_oracle_options_init` initializes defaults without reading secrets.

Requests are stateless, non-streaming, and set `store: false`. A failed send
consumes one attempt; validation failures do not. There are no automatic
retries, sleeps, or model tool execution. Calls and handle access must be
serialized by the host.

## C++: composing with a spider or scraper

```cpp
#include <prowsetk/browser.hpp>
#include <prowsetk/plugins/ai_oracle.hpp>

namespace ai = prowsetk::plugins::ai_oracle;

// browser/session are owned by the crawler. NetworkClient is borrowed and
// must outlive the oracle. Its host transport must not follow redirects.
auto options = ai::options_from_environment();
options.enabled = true;
options.max_requests = 8;
ai::Oracle oracle(browser.network_client(), options);

auto recommendation = oracle.ask_document(
    *session->document(),
    "Recommend the most relevant next link for a product API crawl. "
    "Return a JSON object with next and reason.", ai::Task::Crawl, true);
// recommendation.answer is a validated JSON object string.
// The crawler can match next to its existing bounded, same-origin frontier.
```

`ask(OracleRequest)` additionally accepts `context_json` (a JSON object),
`html`, `page_url`, `images`, `json_output`, and `task` (`Advice`, `Captcha`,
`Crawl`). `ask_document` snapshots the supplied document without mutating it.
`json_output` requests JSON-object output and verifies the returned answer is an
object. Its keys are task-specific; it is not a browser action schema.

The oracle does not add site session cookies or default headers to its API
request. A supplied NetworkClient remains the host's policy/proxy boundary.
The socket client uses certificate-verified HTTPS when OpenSSL is available;
an HTTP-only ProwseTk build still compiles but cannot make a live HTTPS inquiry.

## Lua: CAPTCHA and crawl assistance

From the repository root:

```sh
export LUA_CPATH="$PWD/build/default/plugins/ai-oracle/?.so;;"
# Supply OPENAI_API_KEY through your environment.
```

```lua
local ai = require("ai_oracle")
local oracle, err = ai.new({ enabled = true, max_requests = 8 })
assert(oracle, err)

-- session is the scraper/spider's existing managed lprowse session.
local document = session:document()
local recommendation, err, code = oracle:crawl({
    prompt = "Choose the most useful link for endpoint discovery. " ..
             "Return a JSON object with next and reason.",
    html = document:html(),
    page_url = document:url(),
    context_json = [[{"goal":"product catalog API"}]],
})
assert(recommendation, err)
-- recommendation.answer contains JSON; the driver controls its next action.

-- image_data_url is a caller-supplied data:image/png;base64,... or JPEG data URL.
local candidate, err, code = oracle:captcha({
    prompt = "Read the text in this challenge image and describe uncertainty.",
    images = { image_data_url },
})
assert(candidate, err)
oracle:close()
```

`oracle:ask(request)` defaults to task `advice` and text output.
`oracle:captcha(request)` defaults to task `captcha` and text output.
`oracle:crawl(request)` defaults to task `crawl` and JSON-object output. These
defaults can be overridden with `task` / `json_output`. All return a result
table, or `nil, secret_free_message, error_code` (for example `network_error` or
`resource_limit`). Result fields are `answer`, `response_id`, `model`,
`provenance`, `advisory`, `input_tokens`, and `output_tokens`. `request_count()`
returns attempted sends; `close()` is idempotent, and GC releases the client.

By default Lua uses the core socket NetworkClient, independently of the site's
session. Hosts can supply `ai.new(options, send_callback)` instead. The callback
receives `{method, url, headers, body, timeout_ms, max_response_bytes}` and
returns `{status, body, final_url?, redirect_chain?}`. The host must honor the
specified limits and disable redirects **before** sending. Exceptions and
malformed callback responses return sanitized errors. Reentering or closing the
same client from its active callback is rejected.

A dedicated, credential-free `lprowse` API session may be used as a callback:

```lua
-- api_session is supplied by the host with follow_redirects=false, a request
-- timeout <= oracle.timeout_ms, and a response limit <= max_response_bytes.
local oracle = assert(ai.new({enabled=true}, function(request)
    return api_session:request(request.method, request.url, {
        headers = request.headers, body = request.body,
    })
end))
```

This composition preserves that session's host request hooks. Site sessions
with authentication headers should not be used as the API transport.

## C/native plugin service

`prowsetk/plugins/ai_oracle.h` is self-contained C and exports
`PROWSETK_AI_ORACLE_API_VERSION = 1`. Another native plugin can link the service
library or resolve these functions from `libprowsetk_ai_oracle.so`:

1. Initialize `ProwseTkAiOracleOptions`, then enable it and supply a key.
2. Supply `ProwseTkAiOracleTransport::send`, backed by the host NetworkClient.
3. `prowsetk_ai_oracle_create` returns an opaque owned handle.
4. `prowsetk_ai_oracle_ask` returns a `ProwseTkAiOracleResult` or a ProwseTkStatus.
5. Read `prowsetk_ai_oracle_error` for a fixed diagnostic after failure.
6. Release the handle with `prowsetk_ai_oracle_free`.

The callback borrows the request. Response storage belongs to the host and must
remain valid until the next send or handle destruction; the service copies it
immediately. Result strings belong to the oracle and expire on the next ask or
free. Callbacks must not reenter or destroy their own C handle. C++ exceptions
never cross this interface. The ProwseTk plugin ABI remains at version 2.

## Data and support contract

- HTML is parsed into a detached, bounded Flatworm DOM. Comments/declarations,
  script/style content, private form-control values, editable content, and
  non-allowlisted attributes are removed. Link/action URLs retain default URL
  redaction. The live document and its credentials are not mutated.
- Structured context and JSON answers recursively redact credential-like keys;
  configured API-key echoes and sensitive embedded URL parameters are removed
  from text, including mixed-case schemes and HTML-encoded query separators.
  Arbitrary prose is not a general-purpose secret classifier. Supply
  application-selected context rather than embedding private data in a prompt.
- Embedded PNG/JPEG data URLs are transmitted verbatim, at most four per
  inquiry, within the shared input budget. The plugin checks their encoding
  shape; the API interprets image bytes. It does not fetch remote image URLs,
  render pages, or redact pixels.
- Requests, prompts, page/challenge content, responses, and credentials are never
  written to plugin logs or generated specifications. API error bodies and
  underlying transport diagnostics are omitted from failures.
- JSON nesting is limited to 32 levels. Incomplete/failed responses, refusals,
  tool calls, empty answers, malformed JSON/usage, oversized input/output, and
  redirects are failures. An injected transport must not follow redirects itself.
- CAPTCHA advice is a candidate answer/handling recommendation, not evidence
  of server clearance. A scraper can pass it to its configured captcha-handler
  workflow and check the actual subsequent response. Visual interactive puzzles
  and MFA are not implemented by this service. Crawl advice does not establish
  coverage or bypass the spider's origin/frontier/request policies.

Hermetic unit tests cover the real OpenAIpp encoder, response decoder, redaction,
images, budgets, errors, and C ownership; integration tests cover native loading,
Lua callbacks, managed-session composition, and crawler snapshots. No live API
key or public network is required.
