-- plugins/scrape2oapi/lua/scrape2oapi.lua
-- Lua specification layer for the scrape2oapi plugin.
--
-- The spec is declarative. Callers pass a table of named options; the plugin
-- never logs secrets and redacts them in the generated OpenAPI by default.
--
-- Usage:
--   local scrape2oapi = require("plugins.scrape2oapi.lua.scrape2oapi")
--   -- or via lprowsext when the native plugin is installed:
--   local scrape = require("scrape2oapi")
--   local result = scrape.scrape(session, {
--       url = "https://example.com/",
--       resolve_chain = true,
--       max_depth = 2,
--       output = "build/openapi.yaml"
--   })
--   print(result.openapi_yaml)

local scrape2oapi = {
    _version = "0.1.0",
    _description = "Scrape internal APIs and dump OpenAPI YAML, optionally resolving chains"
}

-- Default API pattern set. Callers may override via spec.api_patterns.
local DEFAULT_PATTERNS = { "/api", "/v1", "/v2", "/v3", "/graphql", "/rest", "/internal", "/data" }

local function is_api_path(path, patterns)
    if path == nil or path == "" then return false end
    patterns = patterns or DEFAULT_PATTERNS
    local lower = string.lower(path)
    for _, pat in ipairs(patterns) do
        if string.find(lower, string.lower(pat), 1, true) ~= nil then return true end
    end
    -- fallback markers from the core extractor
    for _, m in ipairs({ "/api", "/v1", "/v2", "/v3", "/graphql", "/rest", "/rpc", ".json", "/data", "/internal" }) do
        if string.find(lower, m, 1, true) ~= nil then return true end
    end
    return false
end

local function shallow_copy(t)
    local r = {}
    for k, v in pairs(t) do r[k] = v end
    return r
end

-- Normalizes a Lua spec table into ExtractionOptions-compatible fields and
-- scrape2oapi-specific flags. Secrets are never echoed.
local function normalize_spec(spec)
    spec = spec or {}
    local opts = {}
    opts.url = spec.url or ""
    opts.html = spec.html or ""
    opts.base_url = spec.base_url or spec.base or ""
    opts.follow_links = spec.follow_links ~= false
    opts.inspect_scripts = spec.inspect_scripts ~= false
    opts.observe_network = spec.observe_network or false
    opts.infer_schemas = spec.infer_schemas ~= false
    opts.include_provenance = spec.include_provenance ~= false
    opts.redact_secrets = spec.redact_secrets ~= false
    opts.max_depth = tonumber(spec.max_depth) or 2
    if opts.max_depth < 0 then opts.max_depth = 0 end
    opts.max_pages = tonumber(spec.max_pages) or 100
    opts.minimum_confidence = tonumber(spec.minimum_confidence) or 0.50
    opts.openapi_version = spec.openapi_version or "3.1.0"
    opts.api_patterns = spec.api_patterns or shallow_copy(DEFAULT_PATTERNS)
    if type(spec.api_pattern) == "string" and spec.api_pattern ~= "" then
        opts.api_patterns = { spec.api_pattern }
    end
    opts.require_api_pattern = spec.require_api_pattern ~= false
    opts.resolve_chain = spec.resolve_chain == true or spec.resolve == true
    opts.follow_json_links = spec.follow_json_links ~= false
    opts.max_resolve_requests = tonumber(spec.max_resolve_requests) or 64
    opts.output = spec.output or spec.out or ""
    return opts
end

-- Filters a result:endpoints() list to suspected internal APIs.
local function filter_api_endpoints(endpoints, spec)
    local patterns = spec.api_patterns or DEFAULT_PATTERNS
    local require = spec.require_api_pattern ~= false
    if not require then return endpoints end
    local filtered = {}
    for _, ep in ipairs(endpoints) do
        if is_api_path(ep.path, patterns) then
            filtered[#filtered + 1] = ep
        end
    end
    return filtered
end

-- Resolves chain by issuing session:request for each endpoint. Host-mediated;
-- respects the session's cookie jar and redirect policy. Deduplicated by URL.
local function resolve_chain(session, endpoints, spec)
    if not spec.resolve_chain then return endpoints end
    if session == nil or type(session.request) ~= "function" then
        return endpoints
    end
    local visited = {}
    local resolved = {}
    local max = spec.max_resolve_requests or 64
    local count = 0
    local queue = {}
    for _, ep in ipairs(endpoints) do queue[#queue + 1] = { ep = ep, depth = 0 } end

    local function api_in_body(body)
        if type(body) ~= "string" or #body == 0 then return {} end
        local found = {}
        -- naive scan for quoted API-like strings
        for quoted in string.gmatch(body, "\"([^\"]+)\"") do
            if is_api_path(quoted, spec.api_patterns) then
                if string.sub(quoted, 1, 1) == "/" or string.match(quoted, "^https?://") then
                    found[#found + 1] = quoted
                end
            end
        end
        for quoted in string.gmatch(body, "'([^']+)'") do
            if is_api_path(quoted, spec.api_patterns) then
                if string.sub(quoted, 1, 1) == "/" or string.match(quoted, "^https?://") then
                    found[#found + 1] = quoted
                end
            end
        end
        return found
    end

    while #queue > 0 and count < max do
        local item = table.remove(queue, 1)
        local ep = item.ep
        if visited[ep.url] then goto continue end
        visited[ep.url] = true
        local ok, resp = pcall(function() return session:request("GET", ep.url) end)
        if ok and type(resp) == "table" then
            ep.final_url = resp.final_url or ep.url
            ep.status = resp.status
            ep.redirect_chain = resp.redirect_chain
            ep.body = resp.body
            if resp.headers ~= nil and resp.headers["Content-Type"] ~= nil then
                ep.response_content_type = resp.headers["Content-Type"]
            end
            resolved[#resolved + 1] = ep
            count = count + 1
            if spec.follow_json_links ~= false and item.depth < (spec.max_depth or 2) then
                local ct = (resp.headers and resp.headers["Content-Type"]) or ""
                local is_json = string.find(string.lower(ct), "json", 1, true) ~= nil
                if not is_json and type(resp.body) == "string" and #resp.body > 0 and string.sub(resp.body, 1, 1) == "{" then
                    is_json = true
                end
                if is_json then
                    for _, url in ipairs(api_in_body(resp.body)) do
                        if not visited[url] then
                            local ne = {
                                url = url,
                                path = url:match("https?://[^/]+([^?#]*)") or url:match("([^?#]+)") or url,
                                method = "get",
                                source = ep.final_url or ep.url,
                                discovery_method = "resolved-json",
                                confidence = 0.60,
                                parameters = {},
                                notes = { "discovered in JSON response body" }
                            }
                            queue[#queue + 1] = { ep = ne, depth = item.depth + 1 }
                        end
                    end
                end
            end
        else
            ep.resolve_error = (type(resp) == "string" and resp) or "request failed"
            resolved[#resolved + 1] = ep
            count = count + 1
        end
        ::continue::
    end

    -- append any endpoints that were not resolved due to limit
    for _, ep in ipairs(endpoints) do
        if not visited[ep.url] then resolved[#resolved + 1] = ep end
    end
    return resolved
end

-- Primary entrypoint: scrape(session_or_document, spec) -> result
-- result is the native EndpointExtractionResult (with helpers) plus filtered
-- endpoints and resolved chain when applicable.
function scrape2oapi.scrape(session_or_document, spec)
    local opts = normalize_spec(spec)

    local doc = nil
    local session = nil
    if session_or_document ~= nil then
        if type(session_or_document.document) == "function" then
            session = session_or_document
            if opts.html ~= nil and opts.html ~= "" then
                local base = opts.base_url ~= "" and opts.base_url or opts.url
                if base == "" then base = "https://example.com/" end
                session:load_html(opts.html, base)
            elseif opts.url ~= nil and opts.url ~= "" then
                -- Only navigate when session has no document or URL differs.
                local cur = nil
                if type(session.current_url) == "function" then cur = session:current_url() end
                if cur ~= opts.url then
                    session:navigate(opts.url)
                end
            end
            doc = session:document()
        elseif type(session_or_document.title) == "function" then
            doc = session_or_document
        else
            doc = session_or_document
        end
    end

    if doc == nil and session ~= nil then doc = session:document() end
    if doc == nil then
        error("scrape2oapi.scrape: no document available (provide url/html or load a document first)", 2)
    end

    -- Prefer the C++ endpoint extractor via lprowsext for parity with the core.
    local ext = nil
    local ok, mod = pcall(require, "lprowsext")
    if ok then ext = mod end

    local result = nil
    local extraction_opts = {
        follow_links = opts.follow_links,
        inspect_scripts = opts.inspect_scripts,
        observe_network = opts.observe_network,
        infer_schemas = opts.infer_schemas,
        include_provenance = opts.include_provenance,
        redact_secrets = opts.redact_secrets,
        max_depth = opts.max_depth,
        max_pages = opts.max_pages,
        minimum_confidence = opts.minimum_confidence,
        openapi_version = opts.openapi_version
    }

    if ext ~= nil and ext.endpoints ~= nil and type(ext.endpoints.extract) == "function" then
        result = ext.endpoints.extract(doc, extraction_opts)
    else
        error("scrape2oapi: lprowsext.endpoints.extract is not available (build without Lua?)", 2)
    end

    local endpoints = result:endpoints()
    local filtered = filter_api_endpoints(endpoints, opts)

    -- Optionally resolve chains (requires a session)
    local resolved = filtered
    if opts.resolve_chain and session ~= nil then
        resolved = resolve_chain(session, filtered, opts)
    end

    -- Write OpenAPI YAML if requested. For filtered output we regenerate the
    -- YAML manually so non-API paths are not present.
    local function write_yaml(path, content)
        if path == nil or path == "" then return false end
        local dir = path:match("^(.*)/[^/]+$")
        if dir ~= nil and dir ~= "" and dir ~= "." then
            os.execute("mkdir -p '" .. dir:gsub("'", "'\\''") .. "'")
        end
        local f, err = io.open(path, "w")
        if f == nil then error("scrape2oapi: cannot open output: " .. tostring(path) .. " (" .. tostring(err) .. ")", 2) end
        f:write(content)
        f:close()
        return true
    end

    -- Build a filtered YAML if filtering removed endpoints.
    local yaml = result:openapi_yaml()
    if #filtered ~= #endpoints then
        -- Re-render filtered paths by stripping non-API sections from YAML.
        -- Simple heuristic: keep header and only filtered paths.
        -- For correctness we ask the C++ layer to re-render when available via
        -- a helper, but fallback is to reuse full yaml and note filtering.
        -- Here we keep full yaml but emit a warning; callers should use the
        -- native scrape2oapi C++ plugin for exact filtered YAML.
        -- We instead construct a minimal filtered yaml in Lua for determinism.
        local lines = {}
        lines[#lines+1] = "openapi: " .. opts.openapi_version
        lines[#lines+1] = "info:"
        lines[#lines+1] = "  title: 'Discovered API (scrape2oapi)'"
        lines[#lines+1] = "  version: '0.1.0'"
        lines[#lines+1] = "  description: >-"
        lines[#lines+1] = "    Generated by ProwseTk scrape2oapi (Lua). Endpoints are"
        lines[#lines+1] = "    heuristic internal-API discoveries."
        lines[#lines+1] = "x-prowsetk-generated: true"
        lines[#lines+1] = "x-prowsetk-plugin: scrape2oapi"
        if #resolved == 0 then
            lines[#lines+1] = "paths: {}"
        else
            lines[#lines+1] = "paths:"
            -- Group by path
            local by_path = {}
            for _, ep in ipairs(resolved) do
                by_path[ep.path] = by_path[ep.path] or {}
                by_path[ep.path][#by_path[ep.path]+1] = ep
            end
            for path, entries in pairs(by_path) do
                lines[#lines+1] = "  '" .. path:gsub("'", "''") .. "':"
                for _, ep in ipairs(entries) do
                    lines[#lines+1] = "    " .. ep.method .. ":"
                    local op = (ep.method .. "_" .. path):gsub("[^%w]", "_"):gsub("__+", "_"):gsub("^_", ""):gsub("_$", "")
                    op = string.lower(op)
                    lines[#lines+1] = "      operationId: " .. op
                    if ep.parameters ~= nil and #ep.parameters > 0 then
                        lines[#lines+1] = "      parameters:"
                        for _, p in ipairs(ep.parameters) do
                            lines[#lines+1] = "        - name: '" .. p:gsub("'", "''") .. "'"
                            lines[#lines+1] = "          in: query"
                            lines[#lines+1] = "          required: false"
                            lines[#lines+1] = "          schema:"
                            lines[#lines+1] = "            type: string"
                        end
                    end
                    lines[#lines+1] = "      responses:"
                    lines[#lines+1] = "        '200':"
                    lines[#lines+1] = "          description: Inferred response"
                    if opts.infer_schemas then lines[#lines+1] = "      x-inferred: true" end
                    if opts.include_provenance then
                        lines[#lines+1] = "      x-prowsetk-provenance:"
                        lines[#lines+1] = "        source: '" .. (ep.source or ""):gsub("'", "''") .. "'"
                        lines[#lines+1] = "        url: '" .. (ep.url or ""):gsub("'", "''") .. "'"
                        if ep.final_url and ep.final_url ~= ep.url then
                            lines[#lines+1] = "        final-url: '" .. ep.final_url:gsub("'", "''") .. "'"
                        end
                        lines[#lines+1] = "        discovery-method: '" .. (ep.discovery_method or ""):gsub("'", "''") .. "'"
                        lines[#lines+1] = "        confidence: " .. string.format("%.2f", ep.confidence or 0)
                        lines[#lines+1] = "        inferred: true"
                    end
                end
            end
        end
        yaml = table.concat(lines, "\n") .. "\n"
    end

    if opts.output ~= "" then
        write_yaml(opts.output, yaml)
    end

    -- Augmented result table
    local augmented = {
        openapi_yaml = yaml,
        endpoints = resolved,
        all_endpoints = endpoints,
        filtered_endpoints = filtered,
        warnings = result:warnings(),
        endpoint_count = #resolved,
        write_openapi_yaml = function(_, path) return write_yaml(path, yaml) end
    }
    -- Proxy to keep compatibility with lprowsext result helpers
    setmetatable(augmented, {
        __index = function(t, k)
            if k == "openapi_yaml" then return function() return yaml end end
            if k == "endpoint_count" then return function() return #resolved end end
            local v = result[k]
            if type(v) == "function" then
                return function(_, ...) return v(result, ...) end
            end
            return rawget(t, k)
        end
    })
    return augmented
end

-- Convenience: scrape2oapi.dump(session, spec) returns yaml string directly.
function scrape2oapi.dump(session_or_document, spec)
    local r = scrape2oapi.scrape(session_or_document, spec)
    return r.openapi_yaml
end

scrape2oapi.is_api_path = is_api_path
scrape2oapi.normalize_spec = normalize_spec

return scrape2oapi
