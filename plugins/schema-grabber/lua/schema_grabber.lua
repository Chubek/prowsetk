-- plugins/schema-grabber/lua/schema_grabber.lua
-- Lua specification layer for the schema-grabber plugin.
--
-- The spec is declarative. Callers pass a table of named options; the plugin
-- never logs secrets and redacts them in generated output by default. The
-- grabber composes with scrape-endpoints: scrape first for discovery, then
-- enrich the endpoint list with request/response schemas and URL parameters.
--
-- Usage:
--   local grabber = require("plugins.schema-grabber.lua.schema_grabber")
--   local scraped = scrape_endpoints.scrape(session, { url = "https://example.com/" })
--   local result = grabber.enrich(session, {
--       endpoints = scraped.filtered_endpoints,
--       output = "build/openapi-enriched.yaml",
--   })
--   print(result.openapi_yaml)

local schema_grabber = {
    _version = "0.1.0",
    _description = "Reverse-engineer request/response schemas and URL parameters for scraped endpoints",
}

local DEFAULT_PATTERNS = {
    "/api", "/v1", "/v2", "/v3", "/graphql", "/rest", "/internal", "/data",
    "/ajax", "/rpc", "/json", "/xml", "/gateway", "/service", "/backend", "/bff",
    "/dml", "/hotel/hoteladmin", "/partner-settings",
    "/telemetry", "challenge", "/beacon", "/collect",
}

local function lower(value)
    return tostring(value or ""):lower()
end

local function shallow_copy(t)
    local r = {}
    for k, v in pairs(t) do r[k] = v end
    return r
end

local function is_api_path(path, patterns)
    if path == nil or path == "" then return false end
    patterns = patterns or DEFAULT_PATTERNS
    local pl = lower(path)
    for _, pat in ipairs(patterns) do
        if pl:find(lower(pat), 1, true) then return true end
    end
    for _, m in ipairs({ "/api", "/v1", "/v2", "/v3", "/graphql", "/rest",
        "/rpc", ".json", "/data", "/internal", "/hotel/hoteladmin",
        "/partner-settings", "/telemetry", "challenge", "/beacon",
        "/collect" }) do
        if pl:find(m, 1, true) then return true end
    end
    return false
end

local function normalize_spec(spec)
    spec = spec or {}
    local opts = {}
    opts.url = spec.url or ""
    opts.html = spec.html or ""
    opts.base_url = spec.base_url or spec.base or ""
    opts.endpoints = spec.endpoints
    opts.api_patterns = spec.api_patterns or shallow_copy(DEFAULT_PATTERNS)
    if type(spec.api_pattern) == "string" and spec.api_pattern ~= "" then
        opts.api_patterns = { spec.api_pattern }
    end
    opts.require_api_pattern = spec.require_api_pattern ~= false
    opts.inspect_scripts = spec.inspect_scripts ~= false
    opts.infer_schemas = spec.infer_schemas ~= false
    opts.include_provenance = spec.include_provenance ~= false
    opts.include_examples = spec.include_examples ~= false
    opts.redact_secrets = spec.redact_secrets ~= false
    opts.minimum_confidence = tonumber(spec.minimum_confidence) or 0.50
    opts.openapi_version = spec.openapi_version or "3.1.0"
    opts.probe_get_responses = spec.probe_get_responses ~= false
    opts.max_probe_requests = tonumber(spec.max_probe_requests) or 32
    opts.allow_cross_origin = spec.allow_cross_origin == true
        or spec.allow_cross_origin_resolve == true
    opts.max_body_bytes = tonumber(spec.max_body_bytes) or 262144
    opts.max_properties = tonumber(spec.max_properties) or 64
    opts.max_depth = tonumber(spec.max_depth) or 4
    opts.output = spec.output or spec.out or ""
    opts.output_postman = spec.output_postman or spec.postman_output or ""
    opts.collection_name = spec.collection_name or "Discovered API (schema-grabber)"
    return opts
end

local sensitive_names = {}
for _, key in ipairs({ "token", "access_token", "refresh_token", "id_token",
    "api_key", "apikey", "password", "passwd", "secret", "client_secret",
    "session", "sessionid", "auth", "signature", "sig" }) do
    sensitive_names[key] = true
end

local function is_sensitive(name)
    return sensitive_names[lower(name or "")] == true
end

local function infer_scalar(value)
    if value == nil or value == "" then return "string" end
    local lv = lower(value)
    if lv == "true" or lv == "false" then return "boolean" end
    if value:match("^[%+%-]?%d+$") then return "integer" end
    if tonumber(value) ~= nil then return "number" end
    return "string"
end

local function url_decode(value)
    return (tostring(value or ""):gsub("+", " "):gsub("%%(%x%x)", function(hex)
        return string.char(tonumber(hex, 16))
    end))
end

-- Redacts sensitive query-parameter values in a URL for output. Mirrors the
-- engine Redactor policy used by the native plugin; secrets keep their
-- parameter names but lose their values.
local function redact_url_value(url, opts)
    if opts ~= nil and opts.redact_secrets == false then
        return url or ""
    end
    if type(url) ~= "string" then return "" end
    return (url:gsub("([?&])([^&=#?]*)(=)([^&#]*)",
        function(prefix, key, equals, value)
            if is_sensitive(url_decode(key)) then
                return prefix .. key .. equals .. "[REDACTED]"
            end
            return prefix .. key .. equals .. value
        end))
end

local function postman_safe_url(value, opts)
    value = tostring(value or "")
    value = value:gsub("#.*$", "")
    value = value:gsub("^(%a[%w+.-]*://)([^/?#]*)", function(prefix, authority)
        return prefix .. authority:gsub("^.*@", "")
    end)
    return redact_url_value(value, opts)
end

local function query_params_for(url, opts)
    local out, seen = {}, {}
    if type(url) ~= "string" then return out end
    local query = url:match("%?([^#]*)") or ""
    for pair in (query .. "&"):gmatch("(.-)&") do
        if pair ~= "" then
            local raw_name = pair:match("^([^=]*)") or ""
            local raw_value = pair:match("=(.*)$") or ""
            local name = url_decode(raw_name)
            local value = url_decode(raw_value)
            if name ~= "" and not seen[name] then
                seen[name] = true
                local sensitive = is_sensitive(name)
                out[#out + 1] = {
                    name = name,
                    type = infer_scalar(value),
                    required = false,
                    example = (sensitive and opts.redact_secrets ~= false)
                        and "[REDACTED]" or value,
                    sensitive = sensitive,
                }
            end
        end
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

local function looks_like_id(seg)
    if seg == nil or seg == "" then return false end
    if seg:match("^[%+%-]?%d+$") then return true end
    if seg:match("^%x%x%x%x%x%x%x%x%-%x%x%x%x%-%x%x%x%x%-%x%x%x%x%-%x%x%x%x%x%x%x%x%x%x%x%x$") then
        return true
    end
    if #seg >= 20 and seg:match("^[%w%-_]+$") then return true end
    return false
end

local function templatize_path(path)
    if path == nil or path == "" then return "/", {} end
    local params, seen = {}, {}
    local parts = {}
    for seg in (path .. "/"):gmatch("([^/]*)/") do
        if seg:match("^%{.*%}$") then
            parts[#parts + 1] = seg
            local name = seg:sub(2, -2)
            if not seen[name] then
                seen[name] = true
                params[#params + 1] = { name = name, type = "string", example = "" }
            end
        elseif looks_like_id(seg) then
            parts[#parts + 1] = "{id}"
            if not seen["id"] then
                seen["id"] = true
                params[#params + 1] = { name = "id", type = "string", example = seg }
            end
        else
            parts[#parts + 1] = seg
        end
    end
    -- Rebuild preserving leading slash.
    local template = "/" .. table.concat(parts, "/")
    template = template:gsub("//+", "/")
    if #template > 1 then template = template:gsub("/$", "") end
    return template, params
end

local function path_from_url(url)
    return (url or ""):match("^https?://[^/]+([^?#]*)") or url or ""
end

local function url_origin(url)
    return type(url) == "string" and url:match("^(https?://[^/?#]+)") or nil
end

local function same_origin(a, b)
    local oa, ob = url_origin(a), url_origin(b)
    return oa ~= nil and ob ~= nil and oa:lower() == ob:lower()
end

local function filter_to_api(endpoints, opts)
    if not opts.require_api_pattern then return endpoints or {} end
    local out = {}
    for _, ep in ipairs(endpoints or {}) do
        if is_api_path(ep.path or path_from_url(ep.url), opts.api_patterns) then
            out[#out + 1] = ep
        elseif lower(ep.method) ~= "get" then
            out[#out + 1] = ep
        end
    end
    return out
end

local function seed_endpoints(session, doc, opts)
    if opts.endpoints ~= nil then
        return filter_to_api(opts.endpoints, opts)
    end
    local ok, mod = pcall(require, "lprowsext")
    if not ok or mod == nil or mod.endpoints == nil
        or type(mod.endpoints.extract) ~= "function" then
        return {}
    end
    local ok_extract, result = pcall(function()
        return mod.endpoints.extract(doc, {
            follow_links = true,
            inspect_scripts = opts.inspect_scripts,
            observe_network = false,
            infer_schemas = opts.infer_schemas,
            include_provenance = opts.include_provenance,
            redact_secrets = opts.redact_secrets,
            minimum_confidence = opts.minimum_confidence,
            openapi_version = opts.openapi_version,
        })
    end)
    if not ok_extract or result == nil then return {} end
    local ok_list, list = pcall(function() return result:endpoints() end)
    if not ok_list or type(list) ~= "table" then return {} end
    return filter_to_api(list, opts)
end

-- Infers a JSON value schema node (heuristic; redacts sensitive keys).
local function infer_json_value(name, value, opts, depth)
    depth = depth or 0
    local node = { name = name, provenance = "json-response" }
    local t = type(value)
    if t == "boolean" then
        node.type = "boolean"
        node.example = value and "true" or "false"
    elseif t == "number" then
        node.type = (math.type(value) == "integer") and "integer" or "number"
        node.example = tostring(value)
    elseif t == "string" then
        node.type = "string"
        node.example = value
        if is_sensitive(name) and opts.redact_secrets ~= false then
            node.example = "[REDACTED]"
            node.sensitive = true
        end
    elseif t == "table" then
        -- Distinguish arrays from objects.
        local is_array, n = true, 0
        for k, _ in pairs(value) do
            n = n + 1
            if type(k) ~= "number" then is_array = false; break end
        end
        if is_array then
            node.type = "array"
            node.items_type = "string"
            if n > 0 and depth < (opts.max_depth or 4) then
                local kinds = {}
                for _, el in ipairs(value) do kinds[type(el)] = true end
                local count = 0
                for _ in pairs(kinds) do count = count + 1 end
                if count == 1 then
                    local only = next(kinds)
                    node.items_type = only == "number" and "number"
                        or only == "boolean" and "boolean"
                        or only == "table" and "object" or "string"
                end
            end
        else
            node.type = "object"
            node.properties = {}
            for k, v in pairs(value) do
                if #node.properties >= (opts.max_properties or 64) then break end
                node.properties[#node.properties + 1] =
                    infer_json_value(tostring(k), v, opts, depth + 1)
            end
            table.sort(node.properties, function(a, b) return a.name < b.name end)
        end
    else
        node.type = "string"
    end
    return node
end

-- Minimal JSON decode via a bounded recursive parser (no load() of bodies).
local function decode_json(body)
    if type(body) ~= "string" then return nil end
    local pos = 1
    local function skip_ws()
        while pos <= #body and body:sub(pos, pos):match("%s") do pos = pos + 1 end
    end
    local parse_value
    local function parse_string()
        assert(body:sub(pos, pos) == '"', "expected string")
        pos = pos + 1
        local chars = {}
        while pos <= #body do
            local c = body:sub(pos, pos)
            if c == "\\" then
                local e = body:sub(pos + 1, pos + 1)
                if e == "n" then chars[#chars + 1] = "\n"
                elseif e == "t" then chars[#chars + 1] = "\t"
                elseif e == "r" then chars[#chars + 1] = "\r"
                else chars[#chars + 1] = e end
                pos = pos + 2
            elseif c == '"' then
                pos = pos + 1
                return table.concat(chars)
            else
                chars[#chars + 1] = c
                pos = pos + 1
            end
        end
        error("unterminated string")
    end
    local function parse_number()
        local s = body:match("^[%+%-]?%d+%.?%d*[eE]?[%+%-]?%d*", pos)
        pos = pos + #s
        return tonumber(s)
    end
    parse_value = function()
        skip_ws()
        local c = body:sub(pos, pos)
        if c == "{" then
            pos = pos + 1
            local obj = {}
            skip_ws()
            if body:sub(pos, pos) == "}" then pos = pos + 1; return obj end
            while true do
                skip_ws()
                local k = parse_string()
                skip_ws()
                assert(body:sub(pos, pos) == ":", "expected colon")
                pos = pos + 1
                obj[k] = parse_value()
                skip_ws()
                local d = body:sub(pos, pos)
                if d == "," then pos = pos + 1
                elseif d == "}" then pos = pos + 1; break
                else error("expected , or }") end
            end
            return obj
        elseif c == "[" then
            pos = pos + 1
            local arr = {}
            skip_ws()
            if body:sub(pos, pos) == "]" then pos = pos + 1; return arr end
            while true do
                arr[#arr + 1] = parse_value()
                skip_ws()
                local d = body:sub(pos, pos)
                if d == "," then pos = pos + 1
                elseif d == "]" then pos = pos + 1; break
                else error("expected , or ]") end
            end
            return arr
        elseif c == '"' then
            return parse_string()
        elseif body:sub(pos, pos + 3) == "true" then
            pos = pos + 4; return true
        elseif body:sub(pos, pos + 4) == "false" then
            pos = pos + 5; return false
        elseif body:sub(pos, pos + 3) == "null" then
            pos = pos + 4; return nil
        else
            return parse_number()
        end
    end
    local ok, value = pcall(function()
        local v = parse_value()
        skip_ws()
        return v
    end)
    if ok then return value end
    return nil
end

local function schema_from_body(body, opts, provenance)
    if type(body) ~= "string" or body == "" then return nil end
    local first = body:match("^%s*(.)")
    if first ~= "{" and first ~= "[" then return nil end
    local decoded = decode_json(body)
    if type(decoded) ~= "table" then return nil end
    local schema = { type = "object", properties = {}, provenance = provenance or "json-response" }
    if first == "[" then
        schema.type = "array"
        local merged = {}
        for _, el in ipairs(decoded) do
            if type(el) == "table" then
                for k, v in pairs(el) do
                    if merged[k] == nil then merged[k] = v end
                end
            end
        end
        for k, v in pairs(merged) do
            schema.properties[#schema.properties + 1] = infer_json_value(k, v, opts, 1)
        end
    else
        for k, v in pairs(decoded) do
            if #schema.properties >= (opts.max_properties or 64) then break end
            schema.properties[#schema.properties + 1] = infer_json_value(k, v, opts, 1)
        end
    end
    table.sort(schema.properties, function(a, b) return a.name < b.name end)
    return schema
end

local function form_fields_for(doc, endpoint, opts)
    local fields, seen = {}, {}
    if doc == nil or type(doc.forms) ~= "function" then return fields end
    local ok, forms = pcall(function() return doc:forms() end)
    if not ok or type(forms) ~= "table" then return fields end
    local want_method = lower(endpoint.method or "get")
    for _, form in ipairs(forms) do
        local action, method = "", ""
        pcall(function() action = form:attribute("action") or "" end)
        pcall(function() method = lower(form:attribute("method") or "get") end)
        if method == "" then method = "get" end
        local form_path = path_from_url(action ~= "" and action or (endpoint.url or ""))
        if form_path == (endpoint.path or "") and method == want_method then
            local controls = {}
            pcall(function()
                controls = form:query_selector_all(
                    "input[name], select[name], textarea[name]") or {}
            end)
            for _, control in ipairs(controls) do
                local name = ""
                pcall(function() name = control:attribute("name") or "" end)
                if name ~= "" and not seen[name] then
                    seen[name] = true
                    local input_type = ""
                    pcall(function() input_type = lower(control:attribute("type") or "") end)
                    local ftype = "string"
                    if input_type == "number" or input_type == "range" then
                        ftype = "number"
                    elseif input_type == "checkbox" then
                        ftype = "boolean"
                    end
                    local required = false
                    pcall(function()
                        if type(control.has_attribute) == "function" then
                            required = control:has_attribute("required") or false
                        end
                    end)
                    local example = ""
                    pcall(function() example = control:attribute("value") or "" end)
                    local sensitive = is_sensitive(name)
                    if sensitive and opts.redact_secrets ~= false then
                        example = "[REDACTED]"
                    end
                    fields[#fields + 1] = {
                        name = name, type = ftype, required = required,
                        example = example, provenance = "form-field",
                        sensitive = sensitive,
                    }
                end
            end
        end
    end
    table.sort(fields, function(a, b) return a.name < b.name end)
    return fields
end

local function enrich_one(ep, body_info, doc, opts)
    local template, path_params = templatize_path(ep.path or path_from_url(ep.url))
    local enriched = {
        endpoint = shallow_copy(ep),
        path_template = template,
        path_params = path_params,
        query = query_params_for(ep.url, opts),
        form_fields = {},
        request_schema = nil,
        request_provenance = "",
        responses = {},
        response_provenance = "heuristic-default",
    }
    local method = lower(ep.method or "get")
    if doc ~= nil and (method == "post" or method == "put"
        or method == "patch" or method == "delete") then
        enriched.form_fields = form_fields_for(doc, ep, opts)
        if #enriched.form_fields > 0 then
            enriched.request_schema = {
                type = "object", properties = enriched.form_fields,
                provenance = "form-fields",
            }
            enriched.request_provenance = "form-fields"
        end
    end
    if body_info ~= nil and type(body_info.body) == "string"
        and body_info.body ~= "" then
        local ct = body_info.content_type or ep.response_content_type or ""
        local status = body_info.status or 200
        local entry = { status = status, content_type = ct,
            observed = (body_info.status or 0) ~= 0,
            provenance = (body_info.status or 0) ~= 0
                and "observed-response" or "resolved-body" }
        if ct:lower():find("json", 1, true) or body_info.body:match("^%s*{")
            or body_info.body:match("^%s*%[") then
            entry.schema = schema_from_body(body_info.body, opts, entry.provenance)
        end
        enriched.responses[#enriched.responses + 1] = entry
        enriched.response_provenance = entry.provenance
    elseif ep.response_content_type and ep.response_content_type ~= "" then
        enriched.responses[#enriched.responses + 1] = {
            status = 200, content_type = ep.response_content_type,
            observed = false, provenance = "content-type-hint",
        }
        enriched.response_provenance = "content-type-hint"
    else
        enriched.responses[#enriched.responses + 1] = {
            status = 200, content_type = "", observed = false,
            provenance = "heuristic-default",
        }
    end
    return enriched
end

local function yaml_escape(value)
    return "'" .. tostring(value or ""):gsub("'", "''"):gsub("\n", "\\n") .. "'"
end

local function emit_field(lines, field, indent, opts, prefix)
    prefix = prefix or ""
    local pad = string.rep(" ", indent)
    lines[#lines + 1] = pad .. yaml_escape(field.name) .. ":"
    local inner = string.rep(" ", indent + 2)
    if field.type == "object" and field.properties then
        lines[#lines + 1] = inner .. "type: object"
        lines[#lines + 1] = inner .. "properties:"
        for _, sub in ipairs(field.properties) do
            emit_field(lines, sub, indent + 4, opts)
        end
    elseif field.type == "array" then
        lines[#lines + 1] = inner .. "type: array"
        lines[#lines + 1] = inner .. "items:"
        lines[#lines + 1] = inner .. "  type: " .. (field.items_type or "string")
    else
        lines[#lines + 1] = inner .. "type: " .. (field.type or "string")
    end
    if opts.include_examples ~= false and field.example
        and field.example ~= "" then
        local example = field.example
        if field.sensitive and opts.redact_secrets ~= false then
            example = "[REDACTED]"
        end
        lines[#lines + 1] = inner .. "example: " .. yaml_escape(example)
    end
end

local function render_openapi(schemas, opts)
    local lines = {}
    lines[#lines + 1] = "openapi: " .. (opts.openapi_version or "3.1.0")
    lines[#lines + 1] = "info:"
    lines[#lines + 1] = "  title: 'Discovered API (schema-grabber)'"
    lines[#lines + 1] = "  version: '0.1.0'"
    lines[#lines + 1] = "  description: >-"
    lines[#lines + 1] = "    Generated by ProwseTk schema-grabber. Endpoint"
    lines[#lines + 1] = "    request/response schemas and URL parameters are"
    lines[#lines + 1] = "    heuristic reverse-engineering; not authoritative API"
    lines[#lines + 1] = "    documentation. Use alongside scrape-endpoints."
    lines[#lines + 1] = "x-prowsetk-generated: true"
    lines[#lines + 1] = "x-prowsetk-plugin: schema-grabber"
    if #schemas == 0 then
        lines[#lines + 1] = "paths: {}"
        return table.concat(lines, "\n") .. "\n"
    end
    lines[#lines + 1] = "paths:"
    local by_path = {}
    for _, es in ipairs(schemas) do
        local p = es.path_template ~= "" and es.path_template or "/"
        by_path[p] = by_path[p] or {}
        by_path[p][#by_path[p] + 1] = es
    end
    local sorted_paths = {}
    for p in pairs(by_path) do sorted_paths[#sorted_paths + 1] = p end
    table.sort(sorted_paths)
    for _, path in ipairs(sorted_paths) do
        local entries = by_path[path]
        table.sort(entries, function(a, b)
            return (a.endpoint.method or "") < (b.endpoint.method or "")
        end)
        lines[#lines + 1] = "  " .. yaml_escape(path) .. ":"
        for _, es in ipairs(entries) do
            local ep = es.endpoint
            lines[#lines + 1] = "    " .. (ep.method or "get") .. ":"
            local op = ((ep.method or "get") .. "_" .. path)
                :gsub("[^%w]", "_"):gsub("__+", "_")
                :gsub("^_", ""):gsub("_$", ""):lower()
            lines[#lines + 1] = "      operationId: " .. op
            if #es.path_params > 0 or #es.query > 0 then
                lines[#lines + 1] = "      parameters:"
                for _, pp in ipairs(es.path_params) do
                    lines[#lines + 1] = "        - name: " .. yaml_escape(pp.name)
                    lines[#lines + 1] = "          in: path"
                    lines[#lines + 1] = "          required: true"
                    lines[#lines + 1] = "          schema:"
                    lines[#lines + 1] = "            type: string"
                    if opts.include_examples ~= false and pp.example ~= "" then
                        lines[#lines + 1] = "            example: "
                            .. yaml_escape(pp.example)
                    end
                    lines[#lines + 1] = "          x-inferred: true"
                end
                for _, qp in ipairs(es.query) do
                    lines[#lines + 1] = "        - name: " .. yaml_escape(qp.name)
                    lines[#lines + 1] = "          in: query"
                    lines[#lines + 1] = "          required: false"
                    lines[#lines + 1] = "          schema:"
                    lines[#lines + 1] = "            type: " .. (qp.type or "string")
                    if opts.include_examples ~= false and qp.example ~= "" then
                        local example = qp.example
                        if qp.sensitive and opts.redact_secrets ~= false then
                            example = "[REDACTED]"
                        end
                        lines[#lines + 1] = "            example: "
                            .. yaml_escape(example)
                    end
                    lines[#lines + 1] = "          x-inferred: true"
                end
            end
            local req_ct = ep.request_content_type or ""
            if req_ct == "" and es.request_schema ~= nil then
                local m = lower(ep.method)
                if m == "post" or m == "put" or m == "patch" then
                    req_ct = "application/json"
                end
            end
            if es.request_schema ~= nil and req_ct ~= "" then
                lines[#lines + 1] = "      requestBody:"
                lines[#lines + 1] = "        content:"
                lines[#lines + 1] = "          " .. yaml_escape(req_ct) .. ":"
                lines[#lines + 1] = "            schema:"
                lines[#lines + 1] = "              type: object"
                lines[#lines + 1] = "              properties:"
                for _, f in ipairs(es.request_schema.properties or {}) do
                    emit_field(lines, f, 16, opts)
                end
                lines[#lines + 1] = "              x-inferred: true"
            elseif req_ct ~= "" then
                lines[#lines + 1] = "      requestBody:"
                lines[#lines + 1] = "        content:"
                lines[#lines + 1] = "          " .. yaml_escape(req_ct) .. ":"
                lines[#lines + 1] = "            schema:"
                lines[#lines + 1] = "              type: object"
                lines[#lines + 1] = "              x-inferred: true"
            end
            lines[#lines + 1] = "      responses:"
            for _, rs in ipairs(es.responses) do
                lines[#lines + 1] = "        '" .. tostring(rs.status or 200) .. "':"
                lines[#lines + 1] = "          description: "
                    .. (rs.observed and "Observed response" or "Inferred response")
                if rs.content_type and rs.content_type ~= "" then
                    lines[#lines + 1] = "          content:"
                    lines[#lines + 1] = "            "
                        .. yaml_escape(rs.content_type) .. ":"
                    lines[#lines + 1] = "              schema:"
                    if rs.schema ~= nil and rs.schema.properties
                        and #rs.schema.properties > 0 then
                        if rs.schema.type == "array" then
                            lines[#lines + 1] = "                type: array"
                            lines[#lines + 1] = "                items:"
                            lines[#lines + 1] = "                  type: object"
                            lines[#lines + 1] = "                x-inferred: true"
                        else
                            lines[#lines + 1] = "                type: object"
                            lines[#lines + 1] = "                properties:"
                            for _, f in ipairs(rs.schema.properties) do
                                emit_field(lines, f, 18, opts)
                            end
                            lines[#lines + 1] = "                x-inferred: true"
                        end
                    else
                        lines[#lines + 1] = "                type: object"
                        lines[#lines + 1] = "                x-inferred: true"
                    end
                end
            end
            if opts.infer_schemas ~= false then
                lines[#lines + 1] = "      x-inferred: true"
            end
            lines[#lines + 1] = "      x-prowsetk-schema:"
            lines[#lines + 1] = "        path-template: " .. yaml_escape(es.path_template)
            if es.request_provenance ~= "" then
                lines[#lines + 1] = "        request-provenance: "
                    .. yaml_escape(es.request_provenance)
            end
            if es.response_provenance ~= "" then
                lines[#lines + 1] = "        response-provenance: "
                    .. yaml_escape(es.response_provenance)
            end
            lines[#lines + 1] = "        inferred: true"
            if opts.include_provenance ~= false then
                lines[#lines + 1] = "      x-prowsetk-provenance:"
                lines[#lines + 1] = "        source: "
                    .. yaml_escape(redact_url_value(ep.source, opts))
                lines[#lines + 1] = "        url: "
                    .. yaml_escape(redact_url_value(ep.url, opts))
                lines[#lines + 1] = "        discovery-method: "
                    .. yaml_escape(ep.discovery_method or "")
                lines[#lines + 1] = "        confidence: "
                    .. string.format("%.2f", ep.confidence or 0)
                lines[#lines + 1] = "        inferred: true"
            end
        end
    end
    return table.concat(lines, "\n") .. "\n"
end

local function json_quote(value)
    return '"' .. tostring(value or ""):gsub('[%z\1-\31\\"]', function(c)
        if c == '"' or c == '\\' then return '\\' .. c end
        return string.format('\\u%04x', string.byte(c))
    end) .. '"'
end

local function render_postman(schemas, opts)
    local items = {}
    local ordered = {}
    for _, es in ipairs(schemas or {}) do
        ordered[#ordered + 1] = es
    end
    table.sort(ordered, function(a, b)
        if a.path_template ~= b.path_template then
            return a.path_template < b.path_template
        end
        return (a.endpoint.method or "") < (b.endpoint.method or "")
    end)
    for _, es in ipairs(ordered) do
        local ep = es.endpoint
        local method = (ep.method or "get"):upper()
        -- Path variables {id} -> :id; query examples appended. The raw
        -- seed URL may carry secret query values, so the final URL is
        -- redacted exactly like the OpenAPI provenance block.
        local url = (ep.url or es.path_template or "")
        url = url:gsub("%{([^}]+)%}", ":%1")
        for _, qp in ipairs(es.query or {}) do
            local has = url:find("[?&]" .. qp.name:gsub("%W", "%%%1") .. "=")
            if not has then
                local example = qp.example or ""
                if qp.sensitive and opts.redact_secrets ~= false then
                    example = "[REDACTED]"
                end
                url = url .. (url:find("?", 1, true) and "&" or "?")
                    .. qp.name .. "=" .. tostring(example)
            end
        end
        url = postman_safe_url(url, opts)
        local headers = {}
        local req_ct = ep.request_content_type or ""
        if req_ct == "" and es.request_schema ~= nil then
            local m = lower(ep.method)
            if m == "post" or m == "put" or m == "patch" then
                req_ct = "application/json"
            end
        end
        if req_ct ~= "" then
            headers[#headers + 1] = '{"key":"Content-Type","value":'
                .. json_quote(req_ct) .. '}'
        end
        local body = ""
        if es.request_schema ~= nil and req_ct ~= "" then
            if req_ct == "application/x-www-form-urlencoded" then
                local fields = {}
                for _, f in ipairs(es.request_schema.properties or {}) do
                    local val = f.example or ""
                    if f.sensitive and opts.redact_secrets ~= false then
                        val = "[REDACTED]"
                    end
                    fields[#fields + 1] = '{"key":' .. json_quote(f.name)
                        .. ',"value":' .. json_quote(val)
                        .. ',"description":"Inferred field; supply a value"}'
                end
                body = ',"body":{"mode":"urlencoded","urlencoded":['
                    .. table.concat(fields, ",") .. ']}'
            else
                local example = "{"
                local parts = {}
                for _, f in ipairs(es.request_schema.properties or {}) do
                    local val = f.example or ""
                    if f.sensitive and opts.redact_secrets ~= false then
                        val = "[REDACTED]"
                    end
                    if f.type == "boolean" then
                        parts[#parts + 1] = json_quote(f.name) .. ":false"
                    elseif f.type == "integer" or f.type == "number" then
                        parts[#parts + 1] = json_quote(f.name) .. ":0"
                    else
                        parts[#parts + 1] = json_quote(f.name) .. ":"
                            .. json_quote(val ~= "" and val or "string")
                    end
                end
                example = example .. table.concat(parts, ",") .. "}"
                body = ',"body":{"mode":"raw","raw":' .. json_quote(example)
                    .. ',"options":{"raw":{"language":"json"}}}'
            end
        end
        local variables = ""
        if es.path_params ~= nil and #es.path_params > 0 then
            local vars = {}
            for _, pp in ipairs(es.path_params) do
                vars[#vars + 1] = '{"key":' .. json_quote(pp.name)
                    .. ',"value":' .. json_quote(pp.example ~= "" and pp.example or "1")
                    .. ',"description":"Inferred path parameter"}'
            end
            variables = ',"variable":[' .. table.concat(vars, ",") .. ']'
        end
        local description = "Heuristic discovery; inferred schemas. Confidence: "
            .. tostring(ep.confidence or 0)
        items[#items + 1] = '{"name":' .. json_quote(method .. " " .. url)
            .. ',"request":{"method":' .. json_quote(method) .. ',"url":'
            .. json_quote(url) .. ',"description":' .. json_quote(description)
            .. ',"header":[' .. table.concat(headers, ",") .. ']' .. body
            .. variables .. '},"response":[]}'
    end
    local description = "Generated by ProwseTk schema-grabber. Heuristic "
        .. "request/response schemas and URL parameters; not authoritative "
        .. "documentation. Secrets are redacted."
    return '{"info":{"name":'
        .. json_quote(opts.collection_name or "Discovered API (schema-grabber)")
        .. ',"schema":"https://schema.getpostman.com/json/collection/v2.1.0/collection.json",'
        .. '"description":' .. json_quote(description) .. '},"item":['
        .. table.concat(items, ",") .. ']}\n'
end

-- Primary entrypoint: enrich(session_or_document, spec) -> result.
-- spec.endpoints optionally carries scrape-endpoints output; otherwise seeds
-- come from lprowsext.endpoints.extract. Network probing for GET responses
-- requires a live session with :request.
function schema_grabber.enrich(session_or_document, spec)
    local opts = normalize_spec(spec)
    local warnings = {}
    local doc, session = nil, nil
    if session_or_document ~= nil then
        if type(session_or_document.document) == "function" then
            session = session_or_document
            if opts.html ~= nil and opts.html ~= "" then
                local base = opts.base_url ~= "" and opts.base_url or opts.url
                if base == "" then base = "https://example.com/" end
                session:load_html(opts.html, base)
            elseif opts.url ~= nil and opts.url ~= "" then
                local cur = nil
                if type(session.current_url) == "function" then
                    pcall(function() cur = session:current_url() end)
                end
                if cur ~= opts.url then
                    pcall(function() session:navigate(opts.url) end)
                end
            end
            pcall(function() doc = session:document() end)
        elseif type(session_or_document.title) == "function" then
            doc = session_or_document
        end
    end
    if doc == nil and session ~= nil then
        pcall(function() doc = session:document() end)
    end
    if doc == nil then
        error("schema_grabber.enrich: no document available "
            .. "(provide url/html or load a document first)", 2)
    end

    local seeds = seed_endpoints(session, doc, opts)

    -- Bounded host-mediated GET probes for response bodies (GET only).
    -- Same-origin unless allow_cross_origin: never probe a host the seeds
    -- were not drawn from (e.g. a static CDN script host).
    local bodies = {}
    local probe_count = 0
    local probe_base = opts.base_url ~= "" and opts.base_url or opts.url or ""
    if probe_base == "" and doc ~= nil and type(doc.url) == "function" then
        pcall(function() probe_base = doc:url() end)
    end
    if session ~= nil and type(session.request) == "function"
        and opts.probe_get_responses then
        local visited = {}
        for _, ep in ipairs(seeds) do
            if probe_count >= (opts.max_probe_requests or 32) then
                warnings[#warnings + 1] =
                    "probe budget exhausted; some response schemas stay heuristic"
                break
            end
            if lower(ep.method or "get") == "get" and not visited[ep.url] then
                visited[ep.url] = true
                if opts.allow_cross_origin ~= true and probe_base ~= ""
                    and not same_origin(probe_base, ep.url or "") then
                    goto continue
                end
                local ok, resp = pcall(function()
                    return session:request("GET", ep.url)
                end)
                probe_count = probe_count + 1
                if ok and type(resp) == "table" then
                    local ct = ""
                    for k, v in pairs(resp.headers or {}) do
                        if lower(k) == "content-type" then ct = v; break end
                    end
                    bodies[lower(ep.method or "get") .. "\0" .. (ep.path or "")] = {
                        body = resp.body or "",
                        content_type = ct,
                        status = resp.status or 200,
                    }
                end
                ::continue::
            end
        end
    end

    local schemas = {}
    for _, ep in ipairs(seeds) do
        local key = lower(ep.method or "get") .. "\0" .. (ep.path or "")
        schemas[#schemas + 1] = enrich_one(ep, bodies[key], doc, opts)
    end
    -- Deterministic order.
    table.sort(schemas, function(a, b)
        if a.path_template ~= b.path_template then
            return a.path_template < b.path_template
        end
        return (a.endpoint.method or "") < (b.endpoint.method or "")
    end)

    local yaml = render_openapi(schemas, opts)
    local postman = render_postman(schemas, opts)
    local function write_file(path, content)
        if path == nil or path == "" then return false end
        local dir = path:match("^(.*)/[^/]+$")
        if dir ~= nil and dir ~= "" and dir ~= "." then
            os.execute("mkdir -p '" .. dir:gsub("'", "'\\''") .. "'")
        end
        local f, err = io.open(path, "w")
        if f == nil then
            error("schema_grabber: cannot open output: " .. tostring(path)
                .. " (" .. tostring(err) .. ")", 2)
        end
        f:write(content)
        f:close()
        return true
    end
    if opts.output ~= "" then write_file(opts.output, yaml) end
    if opts.output_postman ~= "" then write_file(opts.output_postman, postman) end

    return {
        openapi_yaml = yaml,
        postman_json = postman,
        schemas = schemas,
        endpoints = seeds,
        warnings = warnings,
        probe_count = probe_count,
        schema_count = #schemas,
        write_openapi_yaml = function(_, path) return write_file(path, yaml) end,
        write_postman_json = function(_, path) return write_file(path, postman) end,
    }
end

function schema_grabber.grab(session_or_document, spec)
    return schema_grabber.enrich(session_or_document, spec)
end

schema_grabber.is_api_path = is_api_path
schema_grabber.normalize_spec = normalize_spec
schema_grabber.templatize_path = templatize_path
schema_grabber.query_params_for = query_params_for
schema_grabber.infer_scalar = infer_scalar

return schema_grabber
