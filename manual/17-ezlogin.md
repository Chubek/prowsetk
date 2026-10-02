# Chapter 17: ezlogin

[Manual index](README.md)

## Authentication APIs

ezlogin provides a native credential-injection plugin and a Lua authentication
helper. The helper configures a Session directly and implements form and
Booking.com account-portal OAuth-style flows.

```lua
local login = dofile('plugins/ezlogin/lua/ezlogin.lua')
login.configure(session, {
    mode = 'bearer',
    token = assert(os.getenv('EXAMPLE_API_TOKEN'), 'API token is required')
})
```

The token stays in the session request configuration. Do not print option
tables, default headers, or credential-bearing response data.

| Lua `mode` | Required fields | Result |
|---|---|---|
| `basic` | `username`, `password` | Basic Authorization header |
| `bearer` | `token` | Bearer Authorization header; Lua default mode |
| `api-key` | `token`; optional `name` | Header, default `X-API-Key` |
| `custom-header` | `name`, `value` | Caller-selected header |
| `cookie` | `session_cookie` | Explicit Cookie header |
| `form` | `login_url`, `username`, `password` | Host-mediated HTML form flow |
| `oauth` | `login_url`, `username`, `password`; account token options | Booking account-portal flow |

`configure` returns true or raises an error. Native configuration uses its own
field names, including `api_key_name`, `header`, and `value`.

## Form login

```lua
local login = dofile('plugins/ezlogin/lua/ezlogin.lua')
login.configure(session, {
    mode = 'form',
    login_url = 'https://example.com/login',
    username = assert(os.getenv('EXAMPLE_USER')),
    password = assert(os.getenv('EXAMPLE_PASSWORD')),
    username_field = 'email',
    password_field = 'password',
    csrf_field = '_csrf'
})
```

The helper fetches the login page through Session, loads the response so page
JavaScript can expose DOM forms, chooses relevant controls, includes hidden
fields, and submits a POST. It supports username-first steps and bounded
redirect/step processing. It prefers actual DOM forms, with a raw-markup
fallback for simpler hosts/fixtures. Responses update the session cookie jar.

`form_login(session, options)` and `oauth_login(session, options)` are callable
directly. The helper's accepted-success conditions are flow heuristics; a
production driver should confirm a successful response and an authenticated-only
DOM control before exporting protected data. Crawler and Booking drivers add
that positive-evidence check through `success_selector`.

The `trusted` option accepts a caller-supplied URL trust predicate for form
actions/redirects. Enforce the same origin/allowlist at the transport boundary
when page scripts and every redirect hop must be covered. The standalone
crawler supplies its bounded transport and configured trusted login origins.

## OAuth-style account flow

The shipped helper understands Booking.com's account-portal flow: find an
`op_token`, submit the login name, then the password; `as_token` is an optional
distinct account-flow input. This is a site-specific helper rather than a
general OAuth authorization-code client.

The Booking example first reuses imported cookies, confirms admin-page login,
and only then requires credentials. CAPTCHA material is distinct from OAuth
tokens; MFA/human verification may need an assistant-browser or Beacon workflow.
Read dotenv before resolving credentials and keep its values local to the driver.

## Native plugin use

```cpp
browser.plugins().load_native("build/default/plugins/ezlogin/libprowsetk_ezlogin.so");
browser.plugins().initialize_all();
browser.plugins().configure_all({
    {"mode", "bearer"},
    {"token", token}
});
```

The native before-request hook preserves existing headers and supplies Basic,
Bearer, API-key, custom-header, or explicit cookie credentials. It accepts a
`none` mode; form configuration is accepted but the native hook does not perform
the form login. The live form/OAuth implementation resides in Lua; native
`oauth` mode is not accepted. Load Lua explicitly for that behavior.

Native configuration uses unprefixed keys. The registry passes the same
configuration entries to every configured plugin without rewriting them.
Native configuration is process-global in the current ezlogin plugin. Avoid
using separately configured registry instances as simultaneous independent
account policies. Session-local Lua configuration is the clearer path for
per-session credential selection.

An explicit header credential should be applied only to the intended trusted
requests; it is not automatically domain-scoped like cookie-jar entries.
See Chapter 14 for cookie import and Chapter 24 for crawler authentication.

Reference: [ezlogin notes](../plugins/ezlogin/README.md),
`plugins/ezlogin/lua/ezlogin.lua`, native `plugin_entry.cpp`.

**Next:** [Anti-bot and captcha-handler](18-anti-bot-and-captcha-handler.md).
