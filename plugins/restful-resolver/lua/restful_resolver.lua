-- plugins/restful-resolver/lua/restful_resolver.lua
-- Lua specification layer for the restful-resolver plugin.
--
-- Iteratively resolves scraped endpoints through the owning session until a
-- full RESTful surface (at least one GET and at least one POST) is
-- discovered, or until the round/request budget is exhausted. All network
-- access is host-mediated via session:request; secrets are never logged and
-- are redacted in rendered output by default.
--
-- Usage:
--   local restful = require("plugins.restful-resolver.lua.restful_resolver")
--   local result = restful.resolve(session, {
--       url = "https://admin.booking.com/",
--       endpoints = seed_endpoints,  -- optional; scraped when absent
--       max_rounds = 4,
--       max_requests = 64,
--   })
--   print(result.is_complete, result.has_post, #result.endpoints)

local restful_resolver = {
    _version = "0.1.0",
    _description = "Resolve scraped endpoints until a full RESTful API (GET+POST) is discovered",
}

local DEFAULT_PATTERNS = {
    "/api", "/v1", "/v2", "/v3", "/graphql", "/rest", "/internal", "/data",
    "/ajax", "/rpc", "/json", "/xml", "/gateway", "/service", "/backend", "/bff",
    "/dml", "/hotel/hoteladmin", "/partner-settings",
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
        "/partner-settings" }) do
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
    opts.max_rounds = tonumber(spec.max_rounds or spec.max_depth) or 4
    if opts.max_rounds < 0 then opts.max_rounds = 0 end
    opts.max_requests = tonumber(spec.max_requests or spec.max_resolve_requests) or 64
    if opts.max_requests < 0 then opts.max_requests = 0 end
    opts.allow_cross_origin_resolve = spec.allow_cross_origin_resolve == true
        or spec.allow_cross_origin == true
    opts.follow_json_links = spec.follow_json_links ~= false
    opts.inspect_scripts = spec.inspect_scripts ~= false
    opts.infer_schemas = spec.infer_schemas ~= false
    opts.include_provenance = spec.include_provenance ~= false
    opts.redact_secrets = spec.redact_secrets ~= false
    opts.minimum_confidence = tonumber(spec.minimum_confidence) or 0.45
    opts.openapi_version = spec.openapi_version or "3.1.0"
    return opts
end

local function url_origin(url)
    return type(url) == "string" and url:match("^(https?://[^/?#]+)") or nil
end

local function same_origin(a, b)
    local oa, ob = url_origin(a), url_origin(b)
    return oa ~= nil and ob ~= nil and oa:lower() == ob:lower()
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
        if part == ".." then table.remove(parts)
        elseif part ~= "." and part ~= "" then parts[#parts + 1] = part end
    end
    return prefix .. table.concat(parts, "/")
end

local function path_from_url(url)
    return (url or ""):match("^https?://[^/]+([^?#]*)") or url or ""
end

local function endpoint_key(ep)
    return lower(ep.method or "get") .. "\0" .. (ep.url or ep.path or "")
end

local function has_post(endpoints)
    for _, ep in ipairs(endpoints or {}) do
        if lower(ep.method) == "post" then return true end
    end
    return false
end

local function has_get(endpoints)
    for _, ep in ipairs(endpoints or {}) do
        if lower(ep.method) == "get" then return true end
    end
    return false
end

local function filter_to_api(endpoints, opts)
    if not opts.require_api_pattern then return endpoints or {} end
    local out = {}
    for _, ep in ipairs(endpoints or {}) do
        if is_api_path(ep.path or path_from_url(ep.url), opts.api_patterns) then
            out[#out + 1] = ep
        end
    end
    return out
end

local function header_of(headers, name)
    for k, v in pairs(headers or {}) do
        if lower(k) == lower(name) then return v end
    end
end

-- Extracts API-like quoted URLs from a response body (heuristic).
local function api_urls_in_body(body, base_url, opts)
    local found, seen = {}, {}
    if type(body) ~= "string" or body == "" then return found end
    local function add(candidate)
        if type(candidate) ~= "string" or candidate == "" then return end
        if not is_api_path(candidate, opts.api_patterns) then return end
        local resolved = resolve_url(base_url, candidate) or candidate
        if not resolved then return end
        if not opts.allow_cross_origin_resolve and not same_origin(base_url, resolved) then
            return
        end
        if not seen[resolved] then
            seen[resolved] = true
            found[#found + 1] = resolved
        end
    end
    for quoted in body:gmatch('"([^"]+)"') do add(quoted) end
    for quoted in body:gmatch("'([^']+)'") do add(quoted) end
    for u in body:gmatch("https?://[%w%._~:/%?#%[%]@!$&%(%)%*%+,;=%%%-]+") do add(u) end
    for p in body:gmatch("/[%w%._~/%?#%[%]@!$&%(%)%*%+,;=%%%-]+") do add(p) end
    return found
end

-- Detects POST endpoints declared in an HTML body: form method=post plus
-- fetch/XHR calls with an explicit POST method. Heuristic; never treated as
-- authoritative without the host response.
local function post_endpoints_in_html(body, page_url, opts)
    local out = {}
    if type(body) ~= "string" or body == "" then return out end
    local lowered = body:lower()
    if not lowered:find("<form", 1, true) and not lowered:find("fetch(", 1, true)
        and not lowered:find(".open(", 1, true) then
        return out
    end
    for tag in body:gmatch("<[Ff][Oo][Rr][Mm][^>]*>") do
        local action = tag:match('[Aa][Cc][Tt][Ii][Oo][Nn]%s*=%s*"([^"]+)"')
            or tag:match("[Aa][Cc][Tt][Ii][Oo][Nn]%s*=%s*'([^']+)'")
            or tag:match("[Aa][Cc][Tt][Ii][Oo][Nn]%s*=%s*([^%s>]+)")
        local method = tag:match('[Mm][Ee][Tt][Hh][Oo][Dd]%s*=%s*"([^"]+)"')
            or tag:match("[Mm][Ee][Tt][Hh][Oo][Dd]%s*=%s*'([^']+)'")
            or tag:match("[Mm][Ee][Tt][Hh][Oo][Dd]%s*=%s*([^%s>]+)")
        if lower(method or "") == "post" then
            local url = resolve_url(page_url, action or page_url) or (action or page_url)
            local path = path_from_url(url)
            if not opts.require_api_pattern or is_api_path(path, opts.api_patterns) then
                out[#out + 1] = {
                    url = url, path = path, method = "post",
                    source = page_url, discovery_method = "resolved-html-form",
                    confidence = 0.75, parameters = {},
                    request_content_type = "application/x-www-form-urlencoded",
                    notes = { "heuristically discovered POST form in resolved page" },
                }
            end
        end
    end
    for url in body:gmatch('[Ff][Ee][Tt][Cc][Hh]%s*%(%s*"([^"]+)"[^)]*[Mm][Ee][Tt][Hh][Oo][Dd]%s*"%s*:%s*"[Pp][Oo][Ss][Tt]"') do
        local resolved = resolve_url(page_url, url) or url
        local path = path_from_url(resolved)
        if not opts.require_api_pattern or is_api_path(path, opts.api_patterns) then
            out[#out + 1] = {
                url = resolved, path = path, method = "post",
                source = page_url, discovery_method = "resolved-script-post",
                confidence = 0.65, parameters = {},
                notes = { "heuristically discovered fetch POST in resolved page" },
            }
        end
    end
    for method, url in body:gmatch('%.[Oo][Pp][Ee][Nn]%s*%(%s*"([^"]+)"%s*,%s*"([^"]+)"') do
        if lower(method) == "post" then
            local resolved = resolve_url(page_url, url) or url
            local path = path_from_url(resolved)
            if not opts.require_api_pattern or is_api_path(path, opts.api_patterns) then
                out[#out + 1] = {
                    url = resolved, path = path, method = "post",
                    source = page_url, discovery_method = "resolved-xhr-post",
                    confidence = 0.65, parameters = {},
                    notes = { "heuristically discovered XHR POST in resolved page" },
                }
            end
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

-- Primary entrypoint: resolve(session, spec) -> result.
-- result = { endpoints, resolved, has_get, has_post, is_complete,
--            rounds_used, request_count, warnings }.
function restful_resolver.resolve(session, spec)
    local opts = normalize_spec(spec)
    local warnings = {}

    local doc = nil
    if session ~= nil and type(session.document) == "function" then
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
    elseif session ~= nil and type(session.title) == "function" then
        doc = session
        session = nil
    end
    if doc == nil then
        return {
            endpoints = {}, resolved = {},
            has_get = false, has_post = false, is_complete = false,
            rounds_used = 0, request_count = 0,
            warnings = { "restful-resolver: no document available" },
        }
    end

    local base = opts.base_url ~= "" and opts.base_url or opts.url
    if base == "" then
        pcall(function() base = doc:url() end)
        if base == nil or base == "" then base = "https://example.com/" end
    end

    local seeds = seed_endpoints(session, doc, opts)
    local endpoints, seen = {}, {}
    local resolved = {}
    local function add_endpoint(ep)
        if type(ep) ~= "table" or not ep.url or ep.url == "" then return end
        local key = endpoint_key(ep)
        if not seen[key] then
            seen[key] = true
            endpoints[#endpoints + 1] = ep
        end
    end
    for _, ep in ipairs(seeds) do
        add_endpoint(ep)
        resolved[#resolved + 1] = { endpoint = ep, final_url = ep.url,
            status = 0, round = 0 }
    end

    if has_get(endpoints) and has_post(endpoints) then
        return { endpoints = endpoints, resolved = resolved,
            has_get = true, has_post = true, is_complete = true,
            rounds_used = 0, request_count = 0, warnings = warnings }
    end

    -- Document-only callers (no session request capability) cannot probe.
    if session == nil or type(session.request) ~= "function" then
        warnings[#warnings + 1] =
            "restful-resolver: seed document does not expose GET+POST; no session to probe"
        return { endpoints = endpoints, resolved = resolved,
            has_get = has_get(endpoints), has_post = has_post(endpoints),
            is_complete = false, rounds_used = 0, request_count = 0,
            warnings = warnings }
    end

    local queue, visited = {}, {}
    for _, ep in ipairs(seeds) do
        queue[#queue + 1] = { ep = shallow_copy(ep), round = 0 }
        visited[ep.url] = false
    end
    local request_count, rounds_used = 0, 0

    local function enqueue(url, parent_url, round, template)
        if round > opts.max_rounds then return end
        local resolved_url = resolve_url(parent_url or base, url) or url
        if not opts.allow_cross_origin_resolve
            and not same_origin(base, resolved_url) then
            return
        end
        if visited[resolved_url] ~= nil then return end
        local path = path_from_url(resolved_url)
        if opts.require_api_pattern
            and not is_api_path(path, opts.api_patterns) then
            return
        end
        visited[resolved_url] = false
        local ep = shallow_copy(template or {})
        ep.url = resolved_url
        ep.path = path
        ep.method = ep.method or "get"
        ep.source = parent_url or base
        queue[#queue + 1] = { ep = ep, round = round }
    end

    while #queue > 0 and request_count < opts.max_requests do
        local item = table.remove(queue, 1)
        if item.round > opts.max_rounds then goto continue end
        local fetch_url = resolve_url(base, item.ep.url) or item.ep.url
        if visited[fetch_url] == true then goto continue end
        if not opts.allow_cross_origin_resolve
            and not same_origin(base, fetch_url) then
            visited[fetch_url] = true
            goto continue
        end
        visited[fetch_url] = true
        local ok, resp = pcall(function()
            return session:request("GET", fetch_url)
        end)
        request_count = request_count + 1
        if item.round > rounds_used then rounds_used = item.round end
        if ok and type(resp) == "table" then
            local final_url = resp.final_url or fetch_url
            resolved[#resolved + 1] = { endpoint = item.ep,
                final_url = final_url, status = resp.status,
                content_type = header_of(resp.headers, "content-type") or "",
                redirect_chain = resp.redirect_chain, round = item.round }
            local body = resp.body or ""
            if opts.follow_json_links then
                for _, url in ipairs(api_urls_in_body(body, final_url, opts)) do
                    local path = path_from_url(url)
                    enqueue(url, final_url, item.round + 1, {
                        method = "get", discovery_method = "resolved-json",
                        confidence = 0.60, parameters = {},
                        notes = { "heuristically discovered in a resolved response body" },
                        path = path,
                    })
                    -- merge lightweight GET record immediately so rounds_used
                    -- reflects breadth even before the URL is fetched
                    add_endpoint({ url = url, path = path, method = "get",
                        source = final_url, discovery_method = "resolved-json",
                        confidence = 0.60, parameters = {},
                        notes = { "heuristically discovered in a resolved response body" } })
                end
            end
            for _, post in ipairs(post_endpoints_in_html(body, final_url, opts)) do
                add_endpoint(post)
                enqueue(post.url, final_url, item.round + 1, post)
            end
            if has_get(endpoints) and has_post(endpoints) then break end
        else
            resolved[#resolved + 1] = { endpoint = item.ep,
                final_url = fetch_url, status = 0,
                resolve_error = "request failed", round = item.round }
        end
        ::continue::
    end

    local complete = has_get(endpoints) and has_post(endpoints)
    if not complete then
        warnings[#warnings + 1] =
            string.format("restful-resolver: stopped after %d request(s) across %d round(s) " ..
                "without discovering GET+POST; coverage is heuristic and incomplete",
                request_count, rounds_used)
    end
    return {
        endpoints = endpoints, resolved = resolved,
        has_get = has_get(endpoints), has_post = has_post(endpoints),
        is_complete = complete, rounds_used = rounds_used,
        request_count = request_count, warnings = warnings,
    }
end

restful_resolver.is_api_path = is_api_path
restful_resolver.normalize_spec = normalize_spec
restful_resolver.has_post = has_post
restful_resolver.has_get = has_get
restful_resolver.filter_to_api = filter_to_api

return restful_resolver
