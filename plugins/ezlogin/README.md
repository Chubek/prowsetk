# ezlogin

`ezlogin` is a native ProwseTk plugin that applies authentication at the
host-mediated request boundary, before downstream scrapers run. Configure it
with the plugin registry using `mode` plus the matching fields:

- `basic`: `username`, `password` (RFC 7617 Base64 is generated in the plugin)
- `bearer`: `token`
- `api-key`: `token`, optional `api_key_name` (defaults to `X-API-Key`)
- `custom-header`: `header`, `value`
- `cookie`: `session_cookie` - sets a Cookie header with the session cookie value
- `form`: `login_url`, `username`, `password`, optional `username_field`, `password_field`, `csrf_field`
  - Performs form-based login: fetches the login page, extracts CSRF token, POSTs credentials,
    and automatically configures session cookies for subsequent requests

The plugin never logs credential values. It preserves existing request headers,
and rejects incomplete or unknown configurations. HTML form/OAuth-style flows
are now supported via the `form` mode.

Example native configuration (form mode):

```toml
[[plugins]]
name = "ezlogin"
path = "plugins/libprowsetk_ezlogin.so"

[plugin_config.ezlogin]
mode = "form"
login_url = "https://admin.booking.com/"
username = "${BOOKING_DOTCOM_USER}"
password = "${BOOKING_DOTCOM_PASS}"
username_field = "username"
password_field = "password"
csrf_field = "_csrf"
```

Example Lua usage:

```lua
local ezlogin = require("ezlogin")
ezlogin.configure(session, {
    mode = "form",
    login_url = "https://admin.booking.com/",
    username = os.getenv("BOOKING_DOTCOM_USER"),
    password = os.getenv("BOOKING_DOTCOM_PASS")
})
```

Secrets should be supplied by the host/project secret mechanism rather than
committed to `Prowse.toml`.
