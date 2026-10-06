# Flatworm JSON-RPC module

`rpc` is the first shipped native Flatworm module. It implements bounded
[JSON-RPC 2.0](https://www.jsonrpc.org/specification) protocol helpers through
`Flatworm-Module.h` ABI version 1. Its module version is `1.0.0`.
Functions return ordinary page-owned JavaScript objects/arrays. The export
namespace is frozen.

## Build and load

```sh
cmake --preset default
cmake --build --preset default --target flatworm_module_rpc flatworm_rpc_example
build/default/flatworm-modules/rpc/flatworm_rpc_example \
  build/default/flatworm-modules/rpc/libflatworm_rpc.so
# Prints 42 using an offline MemoryNetworkClient.
```

`PROWSETK_BUILD_FLATWORM_MODULES` defaults to `ON`; set it to `OFF` to omit
the shipped modules and their tests/examples. `PROWSETK_BUILD_EXAMPLES=OFF`
omits the embedder example. The module itself needs only the standalone ABI
header and the C++20 standard library; it has no link-time dependency on
ProwseTk, QuickJS, an HTTP library or an external JSON library. QuickJS is
required for JavaScript execution. The `.so` spelling above is for Linux;
use the platform's generated module-library filename elsewhere.

`cmake --install build/default --prefix "$PWD/build/install"` installs the
library under `${CMAKE_INSTALL_LIBDIR}/prowsetk/flatworm-modules` (normally
`lib/prowsetk/flatworm-modules`). Select it explicitly before creating sessions:

```cpp
#include <prowsetk/browser.hpp>

prowsetk::Browser browser;
browser.modules().load_native("./build/default/flatworm-modules/rpc/libflatworm_rpc.so");
auto session = browser.create_session();
```

Page scripts can then use either form:

```javascript
const rpc = Flatworm.module('rpc');
const call = rpc.request('subtract', [42, 23]);
// {jsonrpc: '2.0', method: 'subtract', params: [42, 23], id: 1}
```

```javascript
// In a static <script type="module"> or evaluate_module:
import {request, notification, batch, parseResponse, correlate} from 'flatworm:rpc';
```

Dynamic `import('flatworm:rpc')` resolves the same installed instance.
Native library loading remains a C++ host operation.

## JavaScript API

| Export | Behavior |
|---|---|
| `request(method, params?, id?)` | Creates a request; omitted/`undefined` ID generates the next per-runtime integer ID, starting at 1 |
| `notification(method, params?)` | Creates a request without an `id`; does not consume an automatic ID |
| `batch(requests)` | Validates and copies a nonempty array of requests/notifications; rejects duplicate request IDs |
| `result(id, value)` | Creates a success response; `null` is a valid result |
| `error(id, code, message, data?)` | Creates an error response; code must be a safe integer, message a string, optional data any JSON value |
| `parseRequest(wireOrValue)` | Parses/validates a request, notification or nonempty request batch |
| `parseResponse(wireOrValue, expectedId?)` | Parses/validates a response or nonempty response batch; an optional expected ID requires a single matching response |
| `correlate(requests, responses?)` | Validates the exchange and returns responses ordered by request order, excluding notifications |

Methods are case-sensitive UTF-8 strings. The `rpc.` prefix is reserved by the
specification and rejected for application methods. `params`, when present,
must be an object or array; use omission/`undefined`, rather than `null`, for
no parameters. JSON values are copied across the ABI, so modifying a helper's
result does not change the input object. Normal JSON serialization of object
arguments invokes getters/`toJSON` and follows JavaScript's rules for nested
`undefined`/nonfinite values; cyclic objects and BigInts can fail conversion.
Required top-level `undefined` values and top-level nonfinite numbers are rejected.

IDs are strings, `null`, or integers within JavaScript's safe integer range
(`-9007199254740991` through `9007199254740991`). Fractional/unsafe numeric IDs
are rejected. Numeric spellings such as `1`, `1.0` and `1e0` correlate as the
same ID; the string `"1"` is a different ID. An explicit `null` ID is a request
ID, not a notification. Explicit IDs do not advance the automatic counter;
callers mixing automatic and explicit numeric IDs must avoid collisions.
Automatic IDs advance only after successful result transfer. Module state
persists across document loads, is independent per session/runtime, and is
released on session closure.

`batch`, `parseRequest`, `parseResponse` and `correlate` accept JSON wire text
or JavaScript values. Pass raw response text to validation to detect duplicate
keys and fractional IDs before JavaScript's JSON parsing rounds numbers or
discards duplicate keys. Parsing helpers perform structural validation;
duplicate-ID exchange policy belongs to `batch` and `correlate`.

Every response must have `jsonrpc: "2.0"`, an `id`, and **exactly one** of
`result` or `error`. Errors require an integer `code` and string `message`.
Valid error responses are returned as data, not thrown as native failures.
Request/response envelopes cannot include the other envelope's reserved fields;
additional extension fields otherwise survive validation.

For correlation, a single request expects a single response; a batch expects
a response array. Unknown, duplicate or missing IDs fail. Notifications expect
no response, including when mixed into a batch. For notification-only exchanges,
omit `responses`, pass `undefined`/`null`, or pass an empty/whitespace HTTP body;
the result is `[]`. A literal JSON `null` body or an empty response array is not
a valid notification response. Correlation always returns an array, even for
a single ordinary request.

Constants:

- `JSONRPC_VERSION = "2.0"`.
- `PARSE_ERROR = -32700`, `INVALID_REQUEST = -32600`.
- `METHOD_NOT_FOUND = -32601`, `INVALID_PARAMS = -32602`, `INTERNAL_ERROR = -32603`.
- `SERVER_ERROR_MIN = -32099`, `SERVER_ERROR_MAX = -32000`.
- `MAX_BATCH_SIZE = 128`, `MAX_JSON_DEPTH = 64`.

## Host-mediated HTTP calls

The native exports are synchronous protocol helpers. The page chooses a
transport using Flatworm's session-mediated `fetch` or `XMLHttpRequest`:

```javascript
import {request, parseResponse} from 'flatworm:rpc';

async function call(endpoint, method, params) {
    const message = request(method, params);
    const http = await fetch(endpoint, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(message)
    });
    if (!http.ok) throw new Error('RPC HTTP failure');
    const response = parseResponse(await http.text(), message.id);
    if (response.error) throw new Error('RPC remote failure');
    return response.result;
}

document.getElementById('answer').textContent = await call('/rpc', 'add', [20, 22]);
```

For a batch, send `JSON.stringify(batch([...]))` and pass the body to
`correlate(messages, await http.text())`. An all-notification POST can have
HTTP status 204 with no response body.

HTTP status checks, credentials, redirect/origin policy and application error
handling belong to the caller/owning Session. Requests use the existing cookie
jar, headers, plugins and BeforeRequest/AfterResponse hooks. Host cancellation
prevents transport. The module owns no socket, background thread, URL policy,
retry loop or server-side dispatcher. Response validation alone does not
establish transport authenticity. The embedder example uses a hermetic host
transport and makes no live requests.

## Bounds and failures

- At most 1 MiB of aggregate string/JSON argument bytes per call and separately
  1 MiB of encoded result bytes, as required by the ABI.
- At most 128 messages per batch and 1024 UTF-8 bytes per method name.
- At most 64 JSON levels below the root and 16,384 JSON values per parsed input
  or encoded output. Envelope construction counts toward output depth/value
  bounds. Object keys do not count as separate values.
- Strict JSON rejects duplicate decoded object keys, malformed numbers, trailing
  bytes, invalid UTF-8 and unpaired Unicode surrogates. Embedded NULs inside
  strings round-trip through JSON escapes.
- JSON numbers must convert to finite binary64 values without conversion
  overflow/underflow. Wire number spellings are retained until result lowering
  into JavaScript. Numeric IDs and error codes must also be exactly integral
  in their decimal spelling; rounding a fractional wire value is not accepted.

Malformed input produces the ABI's generic `TypeError`; bounded input/output
failures produce a generic `RangeError`. Native exceptions are contained at
the C ABI. Argument/result bytes and parse excerpts are never put into native
errors or logs, including exceptions from JSON-conversion getters/`toJSON`.
Protocol payloads themselves are preserved, so caller-supplied params, error
messages and response data may contain private values and remain private data.

The core's ScriptOptions still bound JavaScript execution/conversion/jobs.
Native callbacks are trusted host code; JavaScript memory/time limits cannot
sandbox them. The module separately bounds its parsing, encoding and batch work.
There is no JSON-RPC 1.0 compatibility or automatic execution of remote results.

## Implementation and tests

The implementation separates value-only JSON records (`src/json.hpp`/`json.cpp`),
strict decoding (`json_parser.cpp`), encoding (`json_encoder.cpp`), JSON-RPC
legality/correlation (`protocol.cpp`), and ABI/lifetime adaptation (`module.cpp`,
`entry.cpp`). It never accesses a DOM, QuickJS handle or Lua state.

Registered, timeout-bounded tests live in `tests/unit/test_rpc_module.cpp` and
`tests/integration/test_rpc_module.cpp`:

```sh
ctest --preset default -R 'prowsetk\.(unit|integration)\.rpc\.'
ctest --preset asan -R 'prowsetk\.(unit|integration)\.rpc\.'
```

The native protocol cases run without QuickJS; page-runtime cases skip when it
is unavailable. Tests cover instance isolation/persistence, classic/static/dynamic
imports, Unicode, numeric precision, limits, malformed data, HTTP cookies/headers/
redirects/cancellation, notifications, shuffled batches, errors, and managed Lua
page evaluation, using MemoryNetworkClient only.
