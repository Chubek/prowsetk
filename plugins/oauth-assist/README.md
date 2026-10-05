# OAuth assistance

`ProwseTk::oauth_assist` implements explicit OAuth 2.0 authorization-code + S256
PKCE, refresh and a local credential cache. The ABI-v2 facade is network-free;
C++ callers and `ptk-oauth-assist` invoke the service explicitly.

`third_party/liboauthpp` is the repository's name for **liboauthcpp**, an OAuth
1.0a library. Its percent-encoding helper is reused when available, but it cannot
implement Cloudflare's OAuth 2.0 exchange. OAuth 2.0 state, PKCE and token
processing are implemented by this plugin, using OpenSSL cryptography and the
existing `NetworkClient`. Missing liboauthcpp uses an equivalent percent encoder;
missing OpenSSL disables creation of PKCE transactions, not the rest of ProwseTk.

## Configuration

```toml
[oauth]
client_id = "your-registered-public-client-id"
scopes = "your-provider-approved-scopes offline_access"
authorization_endpoint = "https://dash.cloudflare.com/oauth2/auth"
token_endpoint = "https://dash.cloudflare.com/oauth2/token"
redirect_uri = "http://localhost:8976/oauth/callback"
```

Use a provider-registered public client and its approved redirect URI/scopes.
`PROWSETK_OAUTH_CLIENT_ID` and `PROWSETK_OAUTH_SCOPES` override the corresponding
TOML fields. No Wrangler client identity is silently borrowed. Cloudflare's
Browser Run documentation specifies API tokens with Browser Rendering – Edit;
OAuth access requires a Cloudflare-issued client/scopes that grant equivalent
access. Client registration and permission grants are provider prerequisites,
not created by this plugin. The direct API-token configuration works separately.

## CLI login

```sh
build/default/plugins/oauth-assist/ptk-oauth-assist login --config Prowse.toml
build/default/plugins/oauth-assist/ptk-oauth-assist status --config Prowse.toml
build/default/plugins/oauth-assist/ptk-oauth-assist refresh --config Prowse.toml
build/default/plugins/oauth-assist/ptk-oauth-assist logout --config Prowse.toml
```

Open the printed authorization URL in your own browser, authenticate with the
provider, then paste the **complete redirect URL** into the CLI. Terminal input
echo is disabled. This manual redirect handoff has no local callback listener;
the browser may show a connection error at localhost, whose address bar still
contains the redirect. The provider must support the configured redirect.
Input and the one-use transaction expire after five minutes. EOF cancels login.
Only the authorization URL and value-free status messages are printed; no code,
verifier, access token or refresh token is printed.

The URL contains a random state and PKCE challenge. Completion requires the
exact redirect URI, matching state, no duplicate query fields and an authorization
code. Token requests use HTTPS, form encoding and bounded host transport. HTTP
redirects, non-Bearer token types, invalid expiry and malformed JSON are rejected.
Custom transports must perform one HTTP hop, honor time/size limits, and avoid
logging credentials. Tokens returned by C++ are secret-bearing caller data.

## Persistence

The CLI stores one current credential record at
`$HOME/.cache/ProwseTk/OAuth/token.json`, bound to token endpoint, client ID,
redirect URI and requested scopes. A different configuration cannot reuse it.
The directory is owner-only (`0700`); the token file is `0600`. POSIX descriptor-
relative operations reject symlink traversal, nonregular or multi-link files,
foreign ownership, permissive modes and oversized records. Writes use an
exclusive temporary file, fsync and atomic rename, preserving the old record
on exchange failures and write failures before the rename. Cache storage is plaintext protected by filesystem
permissions, not encryption. `logout` deletes the local record; it does not
revoke the provider's token. Multi-profile storage and device authorization are
not implemented.

C++ `Authorization`, `refresh`, `save`, `load`, `expired` and `logout` mirror
the CLI. The caller may supply another absolute private cache directory; tests
keep theirs under the CTest binary directory. Cache access requires POSIX.
Use only one writer per cache. Browser Run prefers explicit API tokens and
refreshes this cache only when its access token is expired (30-second skew).
No credentials are implicitly attached to Flatworm page requests.
