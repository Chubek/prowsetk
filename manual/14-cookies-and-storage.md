# Chapter 14: Cookies and Storage

[Manual index](README.md)

## Default storage lifetime

A Browser owns MemoryStorage. Its cookie jar is browser-owned and shared by
sessions in that Browser, with domain/path/scheme scope enforced per request.
Local and session key/value stores are keyed by session ID; closing a session
releases those in-memory namespaces. Use separate Browser instances when cookie
isolation between accounts is required.

SessionConfig contains profile/isolation/persistence/reuse settings as a public
configuration model. Those flags do not turn the current default Browser into
a disk-backed profile manager. Restarting a process loses its in-memory session
state unless a host explicitly imports or persists it.

## Cookie semantics

Cookie identity is name/domain/path. The jar supports host-only and domain
cookies, path boundaries, expiration/replacement, Secure and HttpOnly metadata,
SameSite values, Max-Age, and common HTTP-date Expires values. Invalid or
out-of-scope response cookies are ignored. Longer matching paths sort first in
the outgoing header. SameSite metadata is not a claim of full browser
cross-site enforcement.

```cpp
prowsetk::Cookie cookie;
cookie.name = "locale";
cookie.value = "en";
cookie.path = "/";
session->cookies().set(prowsetk::parse_url("https://example.test/"), cookie);
```

Valid Set-Cookie responses update the jar before a followed redirect. Page
`document.cookie` uses that same jar, omits HttpOnly values, and follows the
session origin. Cookie-change events omit raw credentials. Jar `get`, `all`,
and `cookie_header` are raw host APIs, so their values require an explicit
output policy.

An explicit `Cookie` session header is a raw header and does not provide the
jar's automatic per-cookie scope. Prefer browser cookie import for credentials
that must follow domain/path restrictions.

## Browser-exported JSON import

```lua
local imported = session:import_cookies_json('cookies.json')
assert(imported >= 0)
```

The C++ functions are `import_cookies_json` and
`import_cookies_json_file`, accepting a CookieJar and optional default origin.
Supported input shapes are an array of cookie objects or a storage-state object
with a `cookies` array. C++ results contain imported/skipped counts and warnings;
Lua returns the imported count. Warnings do not contain cookie values. The
import is for cookies, not a full browser profile or localStorage restoration.

`prowsetk run --cookies-json FILE` imports before drivers execute. A
`[sessions].cookies_json` project path is resolved under project.root. Crawler
has its own config-relative cookie path. Booking examples try imported sessions
before requesting credentials, then require positive HTTP/DOM login evidence.

## Key/value APIs

```cpp
session->local_storage().set("locale", "en");
const auto locale = session->local_storage().get("locale");
session->session_storage().remove("temporary-state");
```

KeyValueStore also provides `clear` and `keys`; `get` returns an optional string.
Page JS maps `getItem`, `setItem`, `removeItem`, `clear`, `key`, and `length`
onto the host stores. StorageAccessListener reports store/key/operation without
the value. The generic Lua session API does not expose every C++ storage method;
page-script storage can be accessed through supported JavaScript evaluation.

## Persistent and encrypted backends

The session support library provides TCBStorage using Tokyo Cabinet and
EncryptedStorage using libtomcrypt. Link `ProwseTk::session` for these backends.

```cpp
#include <prowsetk/storage.hpp>
auto store = prowsetk::make_tcb_storage("build/profile-store");
auto encrypted = prowsetk::make_encrypted_storage(std::move(store), password);
encrypted->local_storage("account-profile").set("locale", "en");
```

TCBStorage persists cookies and local-storage namespaces; session storage is
in-memory. Stable namespace IDs matter when reopening persistent local data.
EncryptedStorage wraps a selected backend, encrypting values and the cookie
record with AES-GCM and PBKDF2-HMAC-SHA256 key derivation (120,000 iterations).
Keep passwords and key material private; keys/namespace names remain metadata.

These are standalone Storage implementations. The current Browser constructor
creates MemoryStorage and has no public storage-injection setter; a project
`persistent = true` table does not wire these backends into Browser by itself.
Spider's LMDB page/frontier cache and pagewatch's deployment/snapshot files are
separate persistence systems, each with its own restart contract.

Reference: `storage.hpp`, `cookie_import.hpp`, `src/session/`.

**Next:** [Endpoint discovery](15-endpoint-discovery.md).
