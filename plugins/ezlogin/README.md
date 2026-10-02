# ezlogin

`ezlogin` supplies a native request-header plugin and a reusable Lua login
helper. Both use host-mediated session requests. See
[Manual Chapter 17](../../manual/17-ezlogin.md) for the complete authentication
and session-confirmation workflow.

## Native request headers

Configure the initialized plugin registry using unprefixed `mode` plus the
matching fields:

- `basic`: `username`, `password` (RFC 7617 Base64 is generated in the plugin)
- `bearer`: `token`
- `api-key`: `token`, optional `api_key_name` (defaults to `X-API-Key`)
- `custom-header`: `header`, `value`
- `cookie`: `session_cookie` - sets a Cookie header with the session cookie value
- `none`: no credential injection (native default)

The plugin never logs credential values. It preserves existing request headers,
and rejects incomplete or unknown modes. Native `form` configuration is
accepted with `login_url`, `username`, and `password`, but the native hook does
not perform form login. Native `oauth` is rejected. Use the Lua helper for
those flows. Native configuration is currently process-global, so separate
registries do not provide independent simultaneous account configurations.

Example native configuration, using a host-resolved credential:

```cpp
browser.plugins().load_native("build/default/plugins/ezlogin/libprowsetk_ezlogin.so");
browser.plugins().initialize_all();
browser.plugins().configure_all({{"mode", "bearer"}, {"token", token}});
```

`[plugin_config.ezlogin]` is a parsed project table for a host to consume
explicitly; `prowsetk run` does not automatically pass it to configure_all.
TOML strings do not automatically expand environment variables.

## Lua login helper

From the repository root:

```lua
local ezlogin = dofile("plugins/ezlogin/lua/ezlogin.lua")
ezlogin.configure(session, {
    mode = "form",
    login_url = "https://admin.booking.com/",
    username = os.getenv("BOOKING_DOTCOM_USER"),
    password = os.getenv("BOOKING_DOTCOM_PASS")
})
```

Installed helpers live under `share/prowsetk/plugins/ezlogin/`; arrange that
file/module path explicitly. Loading native plugin metadata does not register
an `ezlogin` Lua module.

Lua supports Basic, Bearer, API-key, custom-header, explicit cookie, form, and
Booking.com account-portal OAuth-style modes. Its default is `bearer`. API-key
header selection uses `name`; custom-header uses `name`/`value`.

`form_login` fetches the login page, loads it through the owning session so
Flatworm can execute page JavaScript and expose forms, then POSTs credentials
and retains response cookies. It supports hidden fields, username-first steps,
and bounded redirect/step handling. `oauth_login` uses a Booking `op_token`,
posts the login name first, then submits the password; `as_token` is an optional
distinct account-flow input. These are heuristic site flows, and the caller
must confirm successful HTTP and authenticated-only DOM evidence afterward.

Secrets should be supplied by the host/project secret mechanism rather than
committed to `Prowse.toml`.
