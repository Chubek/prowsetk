# Chapter 37: OAuth Assistance

[Manual index](README.md)

## What oauth-assist provides

`plugins/oauth-assist` supplies OAuth 2.0 authorization-code authentication with
S256 PKCE, token refresh and a private local credential cache. Its public C++ API
is `prowsetk/oauth_assist.hpp`; applications link `ProwseTk::oauth_assist`.
The `ptk-oauth-assist` CLI performs explicit `login`, `refresh`, `status` and
`logout` operations. Loading the ABI-v2 native facade performs none of these.

This service authenticates a host to an OAuth provider. It does not log a
Flatworm page into a website or copy credentials into Session headers/cookies.
[Chapter 17](17-ezlogin.md) covers page/request authentication; [Chapter
36](36-cloudflare-browser-run.md) describes reuse of the Cloudflare token cache.

The repository's `third_party/liboauthpp` points to **liboauthcpp**, an OAuth
1.0a library. Its percent-encoding helper is reused when available. OAuth 2.0
state, PKCE and token processing are implemented separately by this plugin with
OpenSSL and the host `NetworkClient`. If liboauthcpp is absent, a local encoder
is used. OpenSSL is required to create PKCE transactions; the rest of the
toolkit remains buildable without it.

## Provider registration and project configuration

Use a provider-registered **public client**, with an approved redirect URI and
scopes. The plugin does not register an application, borrow Wrangler's client
identity or implement confidential-client secret authentication.

```toml
[oauth]
client_id = "your-registered-public-client-id"
scopes = "your-provider-approved-scopes offline_access"
authorization_endpoint = "https://dash.cloudflare.com/oauth2/auth"
token_endpoint = "https://dash.cloudflare.com/oauth2/token"
redirect_uri = "http://localhost:8976/oauth/callback"
```

Replace the client and scope placeholders with provider-approved values.
`offline_access` is an example refresh-token scope, not a promise that every
provider accepts it or returns a refresh token.

| `[oauth]` field | Default | Environment override |
|---|---|---|
| `client_id` | Empty; required for exchange | `PROWSETK_OAUTH_CLIENT_ID` |
| `scopes` | Empty; required for exchange | `PROWSETK_OAUTH_SCOPES` |
| `authorization_endpoint` | `https://dash.cloudflare.com/oauth2/auth` | None |
| `token_endpoint` | `https://dash.cloudflare.com/oauth2/token` | None |
| `redirect_uri` | `http://localhost:8976/oauth/callback` | None |

Nonempty environment values override the first two fields in the CLI and Browser
Run configuration resolver. The low-level C++ `Authorization` and `refresh`
functions use the supplied `Config` directly; they do not read the environment.
`ProjectConfig::oauth` contains parsed TOML values.

Both provider endpoints must use HTTPS without URL userinfo, queries or
fragments. Redirect URIs may use HTTPS, or HTTP on `localhost` / `127.0.0.1`,
and must have no userinfo, query or fragment before authorization begins.
The redirect must also match the provider's registration.

For Cloudflare Browser Run, the token needs the appropriate account permission.
Its documented API-token permission is Browser Rendering – Edit. OAuth requires
a Cloudflare-approved client/scopes with equivalent access; a successful token
exchange does not verify that grant. Direct API-token authentication remains
available independently of OAuth.

## CLI login and manual browser handoff

```sh
cmake --preset default
cmake --build --preset default --target ptk-oauth-assist
build/default/plugins/oauth-assist/ptk-oauth-assist login --config Prowse.toml
```

Installed builds expose `ptk-oauth-assist` on `PATH`. The CLI uses a working-
directory `Prowse.toml` when present, or the file supplied by `--config`.

1. The CLI creates random state and a PKCE verifier, then prints an authorization
   URL containing the state and S256 challenge.
2. Open that URL in your browser and authenticate with the provider.
3. After authorization, copy the **complete redirect URL** from the address bar.
4. Paste it at the CLI prompt and press Enter. Terminal input echo is disabled.
5. The helper validates the redirect, exchanges the code through HTTPS and saves
   the resulting token record.

There is no local callback listener or automatic browser launcher. A localhost
redirect may display a connection error while leaving the complete callback URL
in the address bar. The configured provider/redirect must support this manual
handoff. Pasting only the code is insufficient because the helper also validates
the redirect URI and state.

Both the transaction and the CLI input wait are bounded to five minutes. EOF
cancels input. An attempted completion consumes the transaction, including a
failed completion; start a new login rather than replaying the old redirect.
Provider passwords remain in the user-controlled browser. The CLI prints no
authorization code, verifier, access token or refresh token.

## Refresh, status and logout

```sh
ptk-oauth-assist status --config Prowse.toml
ptk-oauth-assist refresh --config Prowse.toml
ptk-oauth-assist logout --config Prowse.toml
```

| Command | Behavior |
|---|---|
| `login` | New one-use browser authorization and token exchange; saves on success |
| `status` | Reads the bound local record and prints `authenticated` or `expired` |
| `refresh` | Exchanges its refresh token and saves the replacement record |
| `logout` | Validates the bound record and removes the local token file |

`status` makes no provider request and does not verify account permissions or
remote revocation. Both reported states return a successful exit status when
the record can be read; errors return nonzero. A missing, mismatched or invalid
record is an error rather than an unauthenticated status record.

Expiry uses a 30-second margin. Browser Run refreshes automatically only when
the cached access token is expired according to this check. Explicit `refresh`
requires a refresh token, even if the current access token is still valid.
If the provider rotates the refresh token, the replacement is saved. If it omits
a replacement, the previous refresh token is retained. Failed exchanges do not
replace the cache. There is no background refresh thread or retry loop.

Logout deletes local credentials; it does not revoke the provider's grant.

## Cache location and ownership

The CLI uses:

```text
$HOME/.cache/ProwseTk/OAuth/token.json
```

`HOME` must be set; this path does not use `XDG_CACHE_HOME`. One current record
is stored, bound to the token endpoint, client ID, redirect URI and requested
scope string. Using different values, including a differently written scopes
string, cannot silently reuse that record. Multi-profile storage is not provided.

Cache access uses descriptor-relative filesystem operations. The final directory
must be owner-only (`0700`); the token file must be owner-only (`0600`), regular,
owned by the current effective user and have one hard link. Path traversal
through symlinks is rejected, including symlinked ancestors. Newly created
directories use `0700`; an existing permissive final directory is rejected rather
than silently repaired.

Writes use an exclusive temporary file, fsync and atomic rename. Failures before
the rename preserve the previous token file. Use one writer per cache. The
contents are plaintext credentials protected by filesystem permissions, not an
encrypted keychain. C++ hosts can choose another absolute private cache directory;
cache access requires the platform's supported POSIX implementation.

## Embedding an authorization flow

The host supplies the browser/terminal UI and the network client. This function
accepts a UI callback that displays an authorization URL and securely obtains a
complete redirect URL; it does not implement that UI itself:

```cpp
#include <prowsetk/oauth_assist.hpp>
#include <functional>

prowsetk::oauth_assist::Token authenticate(
    prowsetk::NetworkClient& network,
    const prowsetk::oauth_assist::Config& config,
    const std::function<std::string(std::string_view)>& redirect_prompt) {
    prowsetk::oauth_assist::Authorization flow(config);
    const auto redirect = redirect_prompt(flow.authorization_url());
    auto token = flow.finish(network, redirect);
    prowsetk::oauth_assist::save(
        prowsetk::oauth_assist::default_cache_directory(), config, token);
    return token; // secret-bearing caller data; do not print it
}
```

`Authorization` is noncopyable and carries a one-use transaction. `finish`
returns a `Token` with `access_token`, optional `refresh_token`, and `expires_at`
as Unix epoch seconds. The low-level function does not itself save the token;
the example makes that explicit.

For reuse, load a record and refresh it when necessary:

```cpp
auto directory = prowsetk::oauth_assist::default_cache_directory();
auto token = prowsetk::oauth_assist::load(directory, config);
if (prowsetk::oauth_assist::expired(token)) {
    token = prowsetk::oauth_assist::refresh(network, config, token);
    prowsetk::oauth_assist::save(directory, config, token);
}
```

Use a host network client that performs one HTTP hop and honors response/time
limits. Custom transports must not log form bodies or tokens. The default
socket client provides verified HTTPS; credentials are never automatically
attached to a Flatworm Session.

## Validation, bounds and troubleshooting

Completion requires the exact redirect prefix, matching state, a code, no
provider error and no duplicate decoded query keys. Token responses must have
a Bearer token type, a nonempty access token and a positive integral `expires_in`
no larger than one year. Redirected or failed HTTP responses and malformed JSON
are rejected with value-free errors.

| Resource | Bound |
|---|---|
| Authorization transaction / CLI input wait | Five minutes each |
| Complete redirect URL | 32 KiB |
| Individual validated OAuth text field | 16 KiB |
| Token response / cache record | 64 KiB |
| Token HTTP request timeout | 30,000 ms through the default request settings |

For login failure, check the registered client, scopes and exact redirect URI,
then begin a fresh flow. For a cache failure, check ownership, restrictive modes,
symlinked path components and matching configuration. For refresh failure, check
whether the provider issued a refresh token or requires a fresh authorization.
Device authorization, automatic callback serving, provider grant revocation and
confidential-client authentication are outside this implementation.

```sh
ctest --preset default -R 'oauth-assist|browser-run|remote-protocols'
ctest --preset asan -R 'oauth-assist|browser-run|remote-protocols'
```

Coverage includes PKCE/state validation, rejected redirects and token responses,
refresh rotation/retention, cache ownership and symlink checks, CLI status/logout,
and Browser Run's cache-refresh composition. The tests do not require live
provider credentials.

Reference: [plugin guide](../plugins/oauth-assist/README.md),
[public API](../include/prowsetk/oauth_assist.hpp), and
`plugins/oauth-assist/src/`.

**Return to:** [Manual index](README.md).
