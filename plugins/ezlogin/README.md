# ezlogin

`ezlogin` is a native ProwseTk plugin that applies authentication at the
host-mediated request boundary, before downstream scrapers run. Configure it
with the plugin registry using `mode` plus the matching fields:

- `basic`: `username`, `password` (RFC 7617 Base64 is generated in the plugin)
- `bearer`: `token`
- `api-key`: `token`, optional `api_key_name` (defaults to `X-API-Key`)
- `custom-header`: `header`, `value`

The plugin never logs credential values. It preserves existing request headers,
and rejects incomplete or unknown configurations. HTML form/OAuth-style flows
remain session-level operations; use the shipped `drivers/login.lua` to discover
and submit a form, then load `ezlogin.lua` (or configure the native plugin) for
subsequent scraper requests.

Example native configuration:

```toml
[[plugins]]
name = "ezlogin"
path = "plugins/libprowsetk_ezlogin.so"

[plugin_config.ezlogin]
mode = "bearer"
token = "${LOGIN_TOKEN}"
```

Secrets should be supplied by the host/project secret mechanism rather than
committed to `Prowse.toml`.
