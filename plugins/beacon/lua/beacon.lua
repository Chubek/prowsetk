-- plugins/beacon/lua/beacon.lua
-- Lua specification layer for the Beacon oracle plugin.
--
-- Mirrors the native Flash protocol (plugins/beacon/AGENTS.md): drivers call
-- `flash(request_type, filters)` to create a Flash, the Firefox addon lists
-- seeking Flashes and connects only after explicit user consent, then fulfills
-- page DOM / network / stylesheet / tracepoint data through beacond.
--
-- Secrets (auth tokens, cookies, page credentials) are never logged and are
-- redacted from rendered URLs by default. Coverage is heuristic: a Flash
-- existing does not prove the page was observed.

local beacon = {
    _version = "0.1.0",
    _description = "Firefox Native Messaging oracle (Flash lifecycle)",
}

local REQUEST_TYPES = {
    page_dom = true,
    network_info = true,
    stylesheet = true,
    tracepoint = true,
}

local RESOURCE_TYPES = {
    main_frame = true,
    sub_frame = true,
    stylesheet = true,
    script = true,
    image = true,
    xmlhttprequest = true,
    other = true,
}

local SENSITIVE_QUERY = {
    token = true, access_token = true, refresh_token = true,
    api_key = true, apikey = true, password = true, secret = true,
    session = true, sessionid = true, auth = true,
}

local function lower(value)
    return tostring(value or ""):lower()
end

local function escape(value)
    value = tostring(value or "")
    value = value:gsub("\\", "\\\\")
    value = value:gsub('"', '\\"')
    value = value:gsub("\n", "\\n")
    value = value:gsub("\r", "\\r")
    value = value:gsub("\t", "\\t")
    return value
end

local function redact_url(url)
    if type(url) ~= "string" or url == "" then return url or "" end
    local q = url:find("?", 1, true)
    if q == nil then return url end
    local base = url:sub(1, q)
    local query = url:sub(q + 1)
    local parts, first = {}, true
    for raw in (query .. "&"):gmatch("([^&]*)&") do
        local key = raw:match("^([^=]+)") or raw
        local pair = raw
        if SENSITIVE_QUERY[lower(key)] then
            pair = key .. "=[REDACTED]"
        end
        if not first then base = base .. "&" end
        first = false
        base = base .. pair
    end
    return base
end

function beacon.request_types()
    return { "page_dom", "network_info", "stylesheet", "tracepoint" }
end

function beacon.is_valid_request_type(name)
    return REQUEST_TYPES[lower(name)] == true
end

function beacon.is_valid_resource_type(name)
    return RESOURCE_TYPES[lower(name)] == true
end

local function normalize_filters(filters)
    filters = filters or {}
    local out = {}
    out.url_pattern = filters.url_pattern or filters.url or ""
    out.resource_types = {}
    for _, name in ipairs(filters.resource_types or {}) do
        out.resource_types[#out.resource_types + 1] = tostring(name)
    end
    return out
end

function beacon.validate(filters, request_type)
    if not beacon.is_valid_request_type(request_type or "") then
        return false, "unknown request_type: " .. tostring(request_type)
    end
    local normalized = normalize_filters(filters)
    if normalized.url_pattern == "" then
        return false, "url_pattern must not be empty"
    end
    for _, name in ipairs(normalized.resource_types) do
        if not beacon.is_valid_resource_type(name) then
            return false, "unknown resource_type: " .. tostring(name)
        end
    end
    return true, nil
end

-- Creates a Flash table. `flash_id` may be supplied by tests; otherwise the
-- caller (beacond) assigns the uuid. Never logs secret-bearing values.
function beacon.new_flash(request_type, filters, flash_id)
    local normalized = normalize_filters(filters)
    return {
        flash_id = flash_id or "",
        request_type = lower(request_type or ""),
        filters = normalized,
        status = "seeking",
    }
end

function beacon.render_request(flash, opts)
    opts = opts or {}
    -- The broker must receive the real URL filter. Redact only for a
    -- deliberately requested display/log representation.
    local redact = opts.redact_secrets == true
    local pattern = flash.filters and flash.filters.url_pattern or ""
    if redact then pattern = redact_url(pattern) end
    local resources = {}
    for _, name in ipairs((flash.filters or {}).resource_types or {}) do
        resources[#resources + 1] = '"' .. escape(name) .. '"'
    end
    return '{"type":"flash_request","flash_id":"' .. escape(flash.flash_id) ..
        '","request_type":"' .. escape(flash.request_type) ..
        '","filters":{"url_pattern":"' .. escape(pattern) ..
        '","resource_types":[' .. table.concat(resources, ",") .. "]}}"
end

function beacon.render_connect(flash_id, tab_id)
    return '{"type":"flash_connect","flash_id":"' .. escape(flash_id) ..
        '","tab_id":' .. tostring(tonumber(tab_id) or 0) .. "}"
end

function beacon.render_disconnect(flash_id)
    return '{"type":"flash_disconnect","flash_id":"' ..
        escape(flash_id) .. '"}'
end

function beacon.render_ping()
    return '{"type":"ping"}'
end

function beacon.normalize_spec(spec)
    spec = spec or {}
    return {
        request_type = lower(spec.request_type or spec.type or "page_dom"),
        url_pattern = spec.url_pattern or spec.url or "",
        resource_types = spec.resource_types or { "main_frame" },
        socket_path = spec.socket_path or "/tmp/beacond.sock",
        timeout_ms = tonumber(spec.timeout_ms) or 30000,
        redact_secrets = spec.redact_secrets ~= false,
        require_consent = spec.require_consent ~= false,
    }
end

return beacon
