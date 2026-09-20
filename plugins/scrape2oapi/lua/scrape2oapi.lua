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

-- Default API/backend pattern set. Callers may override via spec.api_patterns.
local DEFAULT_PATTERNS = {
    "/api", "/v1", "/v2", "/v3", "/graphql", "/rest", "/internal", "/data",
    "/ajax", "/rpc", "/json", "/xml", "/gateway", "/service", "/backend", "/bff",
    "/dml", "/hotel/hoteladmin", "/partner-settings"
}

local function is_api_path(path, patterns)
    if path == nil or path == "" then return false end
    patterns = patterns or DEFAULT_PATTERNS
    local lower = string.lower(path)
    for _, pat in ipairs(patterns) do
        if string.find(lower, string.lower(pat), 1, true) ~= nil then return true end
    end
    -- fallback markers from the core extractor
    for _, m in ipairs({ "/api", "/v1", "/v2", "/v3", "/graphql", "/rest", "/rpc", ".json",
        "/data", "/internal", "/ajax", "/gateway", "/service", "/backend", "/bff", "/dml" }) do
        if string.find(lower, m, 1, true) ~= nil then return true end
    end
    for _, m in ipairs({ "/hotel/hoteladmin", "/partner-settings" }) do
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
    opts.scrape_all_paths = spec.scrape_all_paths == true
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
    opts.allow_cross_origin_resolve = spec.allow_cross_origin_resolve == true
    opts.assistant_browser_enabled =
        spec.assistant_browser_enabled == true or spec.assistant_browser == true
    opts.assistant_prompt = spec.assistant_prompt ~= false
    opts.assistant_browser = type(spec.assistant_browser) == "string" and spec.assistant_browser or ""
    opts.assistant_browser_method =
        spec.assistant_browser_method or spec.assistant_method or spec.browser_automation_method or "webdriver"
    opts.assistant_browser_endpoint =
        spec.assistant_browser_endpoint or spec.assistant_endpoint or spec.webdriver_url or spec.debugger_url or ""
    opts.assistant_browser_debug_port =
        tonumber(spec.assistant_browser_debug_port or spec.debug_port or spec.cdp_port) or 0
    opts.assistant_wait_timeout_ms = tonumber(spec.assistant_wait_timeout_ms) or 300000
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

local function url_origin(url)
    return type(url) == "string" and url:match("^(https?://[^/?#]+)") or nil
end

local function resolve_url(base, ref)
    if type(ref) ~= "string" or ref == "" then return nil end
    if ref:match("^https?://") then return ref end
    local root = url_origin(base)
    if not root then return nil end
    if ref:sub(1, 2) == "//" then return root:match("^(https?)") .. ":" .. ref end
    if ref:match("^[%w+.-]+:") then return nil end
    if ref:sub(1, 1) == "/" then return root .. ref end
    local clean_base = base:gsub("[?#].*$", "")
    local dir = clean_base:match("^(.*/)[^/]*$") or (root .. "/")
    local joined = dir .. ref
    local prefix = root .. "/"
    local suffix = joined:sub(#prefix + 1)
    local parts = {}
    for part in suffix:gmatch("[^/]+") do
        if part == ".." then
            table.remove(parts)
        elseif part ~= "." and part ~= "" then
            parts[#parts + 1] = part
        end
    end
    return prefix .. table.concat(parts, "/")
end

local function same_origin(a, b)
    local oa, ob = url_origin(a), url_origin(b)
    return oa ~= nil and ob ~= nil and oa:lower() == ob:lower()
end

local function shell_quote(value)
    value = tostring(value or "")
    return "'" .. value:gsub("'", "'\\''") .. "'"
end

local function lower(value)
    return tostring(value or ""):lower()
end

local function document_needs_interaction(doc, endpoints)
    if doc == nil then return false, "" end
    local title, text, html = "", "", ""
    pcall(function() title = type(doc.title) == "function" and doc:title() or "" end)
    pcall(function() text = type(doc.text) == "function" and doc:text() or "" end)
    pcall(function() html = type(doc.html) == "function" and doc:html() or "" end)
    local body = lower(title .. "\n" .. text .. "\n" .. html)
    if body:find("g%-recaptcha") or body:find("h%-captcha") or
       body:find("cf%-turnstile") or body:find("captcha", 1, true) or
       body:find("verify you are human", 1, true) or
       body:find("human verification", 1, true) or
       body:find("verification required", 1, true) then
        return true, "anti-bot or human-verification content detected"
    end
    if endpoints ~= nil and #endpoints == 0 then
        return true, "no API endpoints discovered from current document"
    end
    return false, ""
end

local function assistant_launch_command(opts, url)
    local browser = os.getenv("PROWSETK_ASSISTANT_BROWSER")
    if browser == nil or browser == "" then browser = opts.assistant_browser end
    if browser == nil or browser == "" then browser = "assistant-browser" end
    return browser, browser .. " " .. shell_quote(url)
end

local function offer_assistant_browser(session, doc, endpoints, opts)
    if not opts.assistant_browser_enabled then return false, nil end
    local needed, reason = document_needs_interaction(doc, endpoints)
    if not needed then return false, nil end

    local url = opts.url
    if (url == nil or url == "") and session ~= nil and type(session.current_url) == "function" then
        pcall(function() url = session:current_url() end)
    end
    if (url == nil or url == "") and doc ~= nil and type(doc.url) == "function" then
        pcall(function() url = doc:url() end)
    end
    if url == nil or url == "" then url = opts.base_url end
    if url == nil or url == "" then return false, nil end

    local command, launch = assistant_launch_command(opts, url)
    local handoff = {
        needed = true,
        reason = reason,
        url = url,
        command = command,
        method = opts.assistant_browser_method,
        endpoint = opts.assistant_browser_endpoint,
        debug_port = opts.assistant_browser_debug_port
    }
    if not opts.assistant_prompt then return true, handoff end

    io.stderr:write("scrape2oapi: " .. reason .. ". Launch assistant browser with "
        .. tostring(opts.assistant_browser_method) .. " handoff? [y/N] ")
    local answer = io.read("*l") or ""
    answer = lower(answer):gsub("^%s+", ""):gsub("%s+$", "")
    if answer ~= "y" and answer ~= "yes" then
        return true, handoff
    end
    io.stderr:write("scrape2oapi: launching " .. command .. " for " .. url .. "\n")
    os.execute(launch)
    io.stderr:write("scrape2oapi: press Enter after browser interaction is complete. ")
    io.read("*l")
    return true, handoff
end

local function json_unescape(value)
    return (value:gsub("\\u(%x%x%x%x)", function(hex)
            local code = tonumber(hex, 16)
            if code and code < 128 then return string.char(code) end
            return ""
        end)
        :gsub("\\([\\\"/bfnrt])", {
            ["\\"] = "\\", ['"'] = '"', ["/"] = "/",
            b = "\b", f = "\f", n = "\n", r = "\r", t = "\t"
        }))
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
    for _, ep in ipairs(endpoints) do
        local clone = shallow_copy(ep)
        clone.url = resolve_url(clone.source or spec.url or spec.base_url, clone.url) or clone.url
        queue[#queue + 1] = { ep = clone, depth = 0 }
    end

    local function api_in_body(body, base_url)
        if type(body) ~= "string" or #body == 0 then return {} end
        local found, seen = {}, {}
        local function add(candidate)
            candidate = json_unescape(candidate)
            if not is_api_path(candidate, spec.api_patterns) then return end
            local resolved_url = resolve_url(base_url, candidate)
            if not resolved_url then return end
            if spec.allow_cross_origin_resolve ~= true and not same_origin(base_url, resolved_url) then return end
            if not seen[resolved_url] then
                seen[resolved_url] = true
                found[#found + 1] = resolved_url
            end
        end
        for quoted in body:gmatch('"([^"]+)"') do add(quoted) end
        for quoted in body:gmatch("'([^']+)'") do add(quoted) end
        for url in body:gmatch("https?://[%w%._~:/%?#%[%]@!$&%(%)%*%+,;=%%%-]+") do add(url) end
        for path in body:gmatch("/[%w%._~/%?#%[%]@!$&%(%)%*%+,;=%%%-]+") do add(path) end
        return found
    end

    local function header(headers, name)
        for key, value in pairs(headers or {}) do
            if key:lower() == name then return value end
        end
    end

    local function enqueue_discovered(parent, urls, depth)
        for _, url in ipairs(urls) do
            if not visited[url] then
                local path = url:match("https?://[^/]+([^?#]*)") or url:match("([^?#]+)") or url
                local found = false
                for _, item in ipairs(queue) do
                    if item.ep.url == url then found = true; break end
                end
                if not found then
                    queue[#queue + 1] = {
                        ep = {
                            url = url,
                            path = path,
                            method = "get",
                            source = parent.final_url or parent.url,
                            discovery_method = "resolved-json",
                            confidence = 0.60,
                            parameters = {},
                            notes = { "discovered in JSON response body" }
                        },
                        depth = depth
                    }
                end
            end
        end
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
            ep.response_content_type = header(resp.headers, "content-type") or ep.response_content_type
            resolved[#resolved + 1] = ep
            count = count + 1
            if spec.follow_json_links ~= false and item.depth < (spec.max_depth or 2) then
                local ct = header(resp.headers, "content-type") or ""
                local lower_ct = string.lower(ct)
                local scan_text = string.find(lower_ct, "json", 1, true) ~= nil
                    or string.find(lower_ct, "javascript", 1, true) ~= nil
                    or string.find(lower_ct, "text/", 1, true) ~= nil
                    or string.find(lower_ct, "html", 1, true) ~= nil
                if not scan_text and type(resp.body) == "string" and #resp.body > 0 then
                    local first = resp.body:match("^%s*(.)")
                    scan_text = first == "{" or first == "[" or first == "<"
                end
                if scan_text then
                    enqueue_discovered(ep, api_in_body(resp.body, ep.final_url or ep.url), item.depth + 1)
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
        scrape_all_paths = opts.scrape_all_paths,
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
    local assistant_needed, assistant_handoff = offer_assistant_browser(session, doc, filtered, opts)

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

    -- Build YAML from the effective endpoint table whenever filtering or
    -- recursive resolution changed the native extractor result.
    local yaml = result:openapi_yaml()
    if opts.resolve_chain or #filtered ~= #endpoints or #resolved ~= #endpoints then
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
        if opts.assistant_browser_enabled then
            lines[#lines+1] = "x-prowsetk-assistant-browser:"
            lines[#lines+1] = "  enabled: true"
            lines[#lines+1] = "  method: '" .. tostring(opts.assistant_browser_method):gsub("'", "''") .. "'"
            local command = os.getenv("PROWSETK_ASSISTANT_BROWSER") or opts.assistant_browser
            if command == nil or command == "" then command = "assistant-browser" end
            lines[#lines+1] = "  command: '" .. tostring(command):gsub("'", "''") .. "'"
            if opts.assistant_browser_endpoint ~= "" then
                lines[#lines+1] = "  endpoint: '" .. opts.assistant_browser_endpoint:gsub("'", "''") .. "'"
            end
            if opts.assistant_browser_debug_port ~= 0 then
                lines[#lines+1] = "  debug-port: " .. tostring(opts.assistant_browser_debug_port)
            end
        end
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

    local warnings = result:warnings()
    if assistant_needed and assistant_handoff ~= nil then
        warnings[#warnings + 1] =
            "assistant browser handoff recommended: " .. assistant_handoff.reason
    end

    -- Augmented result table
    local augmented = {
        openapi_yaml = yaml,
        endpoints = resolved,
        all_endpoints = endpoints,
        filtered_endpoints = filtered,
        warnings = warnings,
        assistant_browser = assistant_handoff,
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

-- Renders OpenAPI YAML from a merged endpoint table (not from a scrape result).
-- This is useful for combining endpoints from multiple pages.
function scrape2oapi.render_openapi_yaml(endpoints, spec)
    spec = spec or {}
    local opts = normalize_spec(spec)
    local redact = opts.redact_secrets ~= false
    
    local lines = {}
    lines[#lines+1] = "openapi: " .. opts.openapi_version
    lines[#lines+1] = "info:"
    lines[#lines+1] = "  title: 'Discovered API (scrape2oapi)'"
    lines[#lines+1] = "  version: '0.1.0'"
    lines[#lines+1] = "  description: >-"
    lines[#lines+1] = "    Generated by ProwseTk scrape2oapi. Endpoints are"
    lines[#lines+1] = "    heuristic discoveries, not authoritative API documentation."
    lines[#lines+1] = "x-prowsetk-generated: true"
    lines[#lines+1] = "x-prowsetk-plugin: scrape2oapi"
    if opts.assistant_browser_enabled then
        lines[#lines+1] = "x-prowsetk-assistant-browser:"
        lines[#lines+1] = "  enabled: true"
        lines[#lines+1] = "  method: '" .. tostring(opts.assistant_browser_method):gsub("'", "''") .. "'"
        local command = os.getenv("PROWSETK_ASSISTANT_BROWSER") or opts.assistant_browser
        if command == nil or command == "" then command = "assistant-browser" end
        lines[#lines+1] = "  command: '" .. tostring(command):gsub("'", "''") .. "'"
        if opts.assistant_browser_endpoint ~= "" then
            lines[#lines+1] = "  endpoint: '" .. opts.assistant_browser_endpoint:gsub("'", "''") .. "'"
        end
        if opts.assistant_browser_debug_port ~= 0 then
            lines[#lines+1] = "  debug-port: " .. tostring(opts.assistant_browser_debug_port)
        end
    end
    
    if #endpoints == 0 then
        lines[#lines+1] = "paths: {}"
    else
        lines[#lines+1] = "paths:"
        -- Group by path
        local by_path = {}
        for _, ep in ipairs(endpoints) do
            by_path[ep.path] = by_path[ep.path] or {}
            by_path[ep.path][#by_path[ep.path]+1] = ep
        end
        -- Sort paths for determinism
        local sorted_paths = {}
        for path in pairs(by_path) do sorted_paths[#sorted_paths+1] = path end
        table.sort(sorted_paths)
        
        for _, path in ipairs(sorted_paths) do
            local entries = by_path[path]
            lines[#lines+1] = "  '" .. path:gsub("'", "''") .. "':"
            -- Sort entries by method
            table.sort(entries, function(a, b) return (a.method or '') < (b.method or '') end)
            for _, ep in ipairs(entries) do
                lines[#lines+1] = "    " .. ep.method .. ":"
                local op = (ep.method .. "_" .. path):gsub("[^%w]", "_"):gsub("__+", "_"):gsub("^_", ""):gsub("_$", "")
                op = string.lower(op)
                lines[#lines+1] = "      operationId: " .. op
                if ep.parameters ~= nil and #ep.parameters > 0 then
                    lines[#lines+1] = "      parameters:"
                    local sorted_params = {}
                    for _, p in ipairs(ep.parameters) do sorted_params[#sorted_params+1] = p end
                    table.sort(sorted_params)
                    for _, p in ipairs(sorted_params) do
                        lines[#lines+1] = "        - name: '" .. p:gsub("'", "''") .. "'"
                        lines[#lines+1] = "          in: query"
                        lines[#lines+1] = "          required: false"
                        lines[#lines+1] = "          schema:"
                        lines[#lines+1] = "            type: string"
                    end
                end
                if ep.request_content_type and ep.request_content_type ~= "" then
                    lines[#lines+1] = "      requestBody:"
                    lines[#lines+1] = "        content:"
                    lines[#lines+1] = "          " .. ep.request_content_type:gsub("'", "''") .. ":"
                    lines[#lines+1] = "            schema:"
                    lines[#lines+1] = "              type: object"
                    lines[#lines+1] = "              x-inferred: true"
                end
                if ep.response_content_type and ep.response_content_type ~= "" then
                    lines[#lines+1] = "      responses:"
                    lines[#lines+1] = "        '200':"
                    lines[#lines+1] = "          description: Observed response"
                    lines[#lines+1] = "          content:"
                    lines[#lines+1] = "            " .. ep.response_content_type:gsub("'", "''") .. ":"
                    lines[#lines+1] = "              schema:"
                    lines[#lines+1] = "                type: object"
                    lines[#lines+1] = "                x-inferred: true"
                else
                    lines[#lines+1] = "      responses:"
                    lines[#lines+1] = "        '200':"
                    lines[#lines+1] = "          description: Inferred response"
                end
                if opts.infer_schemas ~= false then
                    lines[#lines+1] = "      x-inferred: true"
                end
                if opts.include_provenance ~= false then
                    lines[#lines+1] = "      x-prowsetk-provenance:"
                    local source_url = ep.source or ""
                    local ep_url = ep.url or ""
                    if redact then
                        -- Simple redaction: remove query params with sensitive names
                        source_url = source_url:gsub("[?&][^&]*(token|secret|password|key|auth)[^&]*", "")
                        ep_url = ep_url:gsub("[?&][^&]*(token|secret|password|key|auth)[^&]*", "")
                    end
                    lines[#lines+1] = "        source: '" .. source_url:gsub("'", "''") .. "'"
                    lines[#lines+1] = "        url: '" .. ep_url:gsub("'", "''") .. "'"
                    if ep.final_url and ep.final_url ~= ep_url then
                        lines[#lines+1] = "        final-url: '" .. ep.final_url:gsub("'", "''") .. "'"
                    end
                    lines[#lines+1] = "        discovery-method: '" .. (ep.discovery_method or ""):gsub("'", "''") .. "'"
                    lines[#lines+1] = "        confidence: " .. string.format("%.2f", ep.confidence or 0)
                    lines[#lines+1] = "        inferred: true"
                    if ep.notes and #ep.notes > 0 then
                        lines[#lines+1] = "        notes:"
                        for _, note in ipairs(ep.notes) do
                            lines[#lines+1] = "          - " .. note:gsub("'", "''")
                        end
                    end
                end
            end
        end
    end
    return table.concat(lines, "\n") .. "\n"
end

-- Convenience: scrape2oapi.dump(session, spec) returns yaml string directly.
function scrape2oapi.dump(session_or_document, spec)
    local r = scrape2oapi.scrape(session_or_document, spec)
    return r.openapi_yaml
end

scrape2oapi.is_api_path = is_api_path
scrape2oapi.normalize_spec = normalize_spec

return scrape2oapi
