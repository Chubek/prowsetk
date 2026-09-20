-- Postman Collection v2.1 exporter. Discovery uses the shipped scrape2oapi
-- Lua layer; network resolution remains explicitly opt-in and host-mediated.
local ok, discovery = pcall(require, "scrape2oapi")
if not ok then discovery = require("plugins.scrape2oapi.lua.scrape2oapi") end
local M = { _version = "0.1.0" }

local function quote(value)
    return '"' .. tostring(value):gsub('[%z\1-\31\\"]', function(c)
        if c == '"' or c == '\\' then return '\\' .. c end
        return string.format('\\u%04x', string.byte(c))
    end) .. '"'
end

local sensitive = {}
for _, key in ipairs({"token", "access_token", "refresh_token", "id_token",
    "api_key", "apikey", "password", "passwd", "secret", "client_secret",
    "session", "sessionid", "auth", "signature", "sig"}) do sensitive[key] = true end

local function safe_url(value, redact)
    value = value or ""
    if not redact then return value end
    value = value:gsub("#.*$", "")
    value = value:gsub("^(%a[%w+.-]*://)([^/?#]*)", function(prefix, authority)
        return prefix .. authority:gsub("^.*@", "")
    end)
    local base, query = value:match("^(.-)%?(.*)$")
    if not base then return value end
    local pairs = {}
    for pair in (query .. "&"):gmatch("(.-)&") do
        local key = pair:match("^([^=]*)")
        local decoded = key:gsub("+", " "):gsub("%%(%x%x)", function(hex)
            return string.char(tonumber(hex, 16))
        end):lower()
        pairs[#pairs + 1] = sensitive[decoded] and (key .. "=[REDACTED]") or pair
    end
    return base .. "?" .. table.concat(pairs, "&")
end

-- Render endpoint tables from lprowsext/scrape2oapi. Unknown body samples,
-- headers, notes and errors are intentionally not serialized.
function M.render_postman_json(endpoints, spec)
    spec = spec or {}
    local redact = spec.redact_secrets ~= false
    local ordered, seen = {}, {}
    for _, ep in ipairs(endpoints) do
        local method = (ep.method or "get"):upper()
        local key = (ep.url or "") .. "\0" .. method
        if not seen[key] then
            seen[key] = true
            ordered[#ordered + 1] = {key = key, ep = ep, method = method}
        end
    end
    table.sort(ordered, function(a, b) return a.key < b.key end)
    local items = {}
    for _, entry in ipairs(ordered) do
        local ep, method = entry.ep, entry.method
        local request_url = ep.url or ""
        if ep.request_content_type ~= "application/x-www-form-urlencoded" then
            local names = {}
            local query = request_url:match("%?([^#]*)") or ""
            for pair in (query .. "&"):gmatch("(.-)&") do
                local key = pair:match("^([^=]*)"):gsub("%%(%x%x)", function(hex)
                    return string.char(tonumber(hex, 16))
                end)
                names[key] = true
            end
            local fragment = request_url:match("(#.*)$") or ""
            request_url = request_url:gsub("#.*$", "")
            for _, name in ipairs(ep.parameters or {}) do
                if not names[name] then
                    local encoded = name:gsub("[^%w._~-]", function(c)
                        return string.format("%%%02X", string.byte(c))
                    end)
                    request_url = request_url .. (request_url:find("?", 1, true) and "&" or "?") .. encoded .. "="
                    names[name] = true
                end
            end
            request_url = request_url .. fragment
        end
        local url = safe_url(request_url, redact)
        local confidence = tonumber(ep.confidence) or 0
        if confidence ~= confidence then confidence = 0 end
        confidence = math.max(0, math.min(1, confidence))
        local description = "Heuristic discovery; inferred request. Confidence: " .. tostring(confidence)
        if spec.include_provenance ~= false then
            description = description .. "\nSource: " .. safe_url(ep.source, redact)
                .. "\nDiscovery method: " .. (ep.discovery_method or "")
            if ep.final_url then description = description .. "\nFinal URL: " .. safe_url(ep.final_url, redact) end
            for _, hop in ipairs(ep.redirect_chain or {}) do
                description = description .. "\nRedirect: " .. safe_url(hop, redact)
            end
            if ep.status then description = description .. "\nObserved HTTP status: " .. tostring(ep.status) end
            if ep.resolve_error then description = description .. "\nResolution failed." end
        end
        local headers = {}
        if ep.request_content_type and ep.request_content_type ~= "" then
            headers[1] = '{"key":"Content-Type","value":' .. quote(ep.request_content_type) .. '}'
        end
        local body = ""
        if ep.request_content_type == "application/x-www-form-urlencoded" then
            local fields = {}
            for _, param in ipairs(ep.parameters or {}) do
                fields[#fields + 1] = '{"key":' .. quote(param)
                    .. ',"value":"","description":"Inferred field; supply a value"}'
            end
            body = ',"body":{"mode":"urlencoded","urlencoded":[' .. table.concat(fields, ",") .. ']}'
        end
        items[#items + 1] = '{"name":' .. quote(method .. " " .. url)
            .. ',"request":{"method":' .. quote(method) .. ',"url":' .. quote(url)
            .. ',"description":' .. quote(description) .. ',"header":['
            .. table.concat(headers, ",") .. ']' .. body .. '},"response":[]}'
    end
    local description = "Generated by ProwseTk scrape2postman. Heuristic API discoveries; "
        .. "not authoritative documentation. Request bodies and credentials are omitted."
    if spec.assistant_browser_enabled == true or spec.assistant_browser == true then
        description = description .. " Assistant browser handoff is enabled via "
            .. tostring(spec.assistant_browser_method or spec.assistant_method or "webdriver")
            .. "; any user-assisted coverage remains heuristic."
    end
    return '{"info":{"name":' .. quote(spec.collection_name or "Discovered API (scrape2postman)")
        .. ',"schema":"https://schema.getpostman.com/json/collection/v2.1.0/collection.json",'
        .. '"description":' .. quote(description) .. '},"item":['
        .. table.concat(items, ",") .. ']}\n'
end

local function write(path, json)
    local file = assert(io.open(path, "wb"), "scrape2postman: cannot open output")
    local success = file:write(json)
    local closed = file:close()
    assert(success and closed, "scrape2postman: cannot write output")
    return true
end

function M.scrape(session_or_document, spec)
    spec = spec or {}
    local opts = {}
    for key, value in pairs(spec) do opts[key] = value end
    if spec.recursive == true then opts.resolve_chain = true end
    -- Never let the discovery layer write an intermediate OpenAPI document.
    opts.output, opts.out = "", ""
    local result = discovery.scrape(session_or_document, opts)
    local json = M.render_postman_json(result.endpoints, spec)
    local output = spec.output or spec.out
    if output and output ~= "" then write(output, json) end
    return {
        postman_json = json, endpoints = result.endpoints,
        endpoint_count = #result.endpoints, warnings = result.warnings,
        assistant_browser = result.assistant_browser,
        write_postman_json = function(_, path) return write(path, json) end
    }
end

function M.dump(session_or_document, spec)
    return M.scrape(session_or_document, spec).postman_json
end
return M
