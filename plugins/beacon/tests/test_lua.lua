local beacon = dofile(arg[1])
local flash = beacon.new_flash("page_dom", {
    url_pattern = "https://example.test/page?api_key=private",
    resource_types = { "main_frame" },
}, "11111111-1111-4111-8111-111111111111")
local ok = beacon.validate(flash.filters, flash.request_type)
assert(ok)
local wire = beacon.render_request(flash)
assert(wire:find("api_key=private", 1, true))
local display = beacon.render_request(flash, { redact_secrets = true })
assert(not display:find("private", 1, true))
assert(display:find("[REDACTED]", 1, true))
assert(not beacon.validate({ url_pattern = "https://example.test/*",
    resource_types = { "unknown" } }, "page_dom"))
