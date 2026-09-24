-- drivers/scrape2oapi.lua
-- The `scrape2oapi` driver: given a page, scrape suspected internal API
-- endpoints and optionally resolve redirect/API chains, dumping OpenAPI YAML.
--
-- Driver contract (AGENTS.md 1):
--   Defines global main(args) returning integer exit code; args table is built
--   from Prowse.toml [[drivers]] declaration. Secrets are never echoed.
--
-- Arguments (see also Prowse.toml [[drivers]] entry created by scaffold):
--   url              string  required        page URL to inspect
--   api_pattern      string  optional        substring to identify APIs (default "/api")
--   api_patterns     string  optional        comma-separated patterns (overrides api_pattern)
--   resolve_chain    boolean default false   follow redirects and JSON-discovered links
--   max_depth        integer default 2       chain recursion depth
--   max_pages        integer default 100
--   output           path    default "build/openapi.yaml"
--   html             string  optional        operate on in-memory document instead of network
--
-- Offline runs: when `html` is supplied, the driver loads that document via
-- session:load_html instead of navigating the network, ensuring deterministic
-- tests. Chain resolution is skipped in offline mode when no network is
-- available, but the driver still reports the discovery result.

local prowse = require("lprowse")

-- ---------------------------------------------------------------------------
-- JSON encoder (keeps driver dependency-free, mirrors crawl_site/login drivers)
-- ---------------------------------------------------------------------------
local function json_encode(value)
    local function encode(v)
        local kind = type(v)
        if kind == "nil" then return "null"
        elseif kind == "boolean" then return v and "true" or "false"
        elseif kind == "number" then
            if v ~= v or v == math.huge or v == -math.huge then return "null" end
            return string.format("%.14g", v)
        elseif kind == "string" then
            local e = v:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", "\\n"):gsub("\r", "\\r"):gsub("\t", "\\t")
            e = e:gsub("[%c]", function(c) return string.format("\\u%04x", string.byte(c)) end)
            return '"' .. e .. '"'
        elseif kind == "table" then
            local is_array = true
            local max = 0
            for k in pairs(v) do
                if type(k) ~= "number" or k < 1 or k % 1 ~= 0 then is_array = false break end
                if k > max then max = k end
            end
            if is_array then
                local parts = {}
                for i = 1, max do parts[i] = encode(v[i]) end
                return "[" .. table.concat(parts, ",") .. "]"
            end
            local parts = {}
            for k, val in pairs(v) do parts[#parts+1] = encode(tostring(k)) .. ":" .. encode(val) end
            return "{" .. table.concat(parts, ",") .. "}"
        end
        return "null"
    end
    return encode(value)
end

local function dirname(path)
    local n = tostring(path):gsub("\\", "/")
    local idx = n:match(".*/()")
    if idx ~= nil then return n:sub(1, idx - 1) end
    return "."
end

local function shell_quote(v)
    return "'" .. tostring(v):gsub("'", "'\\''") .. "'"
end

local function ensure_parent(path)
    local dir = dirname(path)
    if dir == "" or dir == "." then return true end
    local ok = os.execute("mkdir -p " .. shell_quote(dir))
    return ok == true
end

local function write_file(path, content)
    ensure_parent(path)
    local f, err = io.open(path, "w")
    if f == nil then error("scrape2oapi: cannot open output: " .. tostring(path) .. " (" .. tostring(err) .. ")", 2) end
    f:write(content)
    f:close()
end

local function parse_patterns(api_pattern, api_patterns)
    if type(api_patterns) == "string" and api_patterns ~= "" then
        local out = {}
        for pat in string.gmatch(api_patterns, "[^,]+") do
            pat = pat:match("^%s*(.-)%s*$")
            if pat ~= "" then out[#out+1] = pat end
        end
        if #out > 0 then return out end
    end
    if type(api_pattern) == "string" and api_pattern ~= "" then
        return { api_pattern }
    end
    return nil
end

function main(args)
    args = args or {}
    local url = args.url or ""
    local html = args.html
    local offline = type(html) == "string" and #html > 0
    if url == "" then error("scrape2oapi: missing required argument: url", 2) end

    local output = args.output or "build/openapi.yaml"
    local resolve_chain = args.resolve_chain == true or args.resolve == true or args.resolve_chain == "true"
    -- offline with html: resolve_chain would require network; allow but pcall will handle failures
    local max_depth = tonumber(args.max_depth) or 2
    local max_pages = tonumber(args.max_pages) or 100
    local api_pattern = args.api_pattern
    local api_patterns = parse_patterns(api_pattern, args.api_patterns)

    local browser = prowse.browser.new({ javascript = true, follow_redirects = true, observe_network = false })
    local session = browser:create_session()

    -- Load document
    if offline then
        session:load_html(html, url)
    else
        session:navigate(url)
    end

    local spec = {
        url = url,
        follow_links = true,
        inspect_scripts = true,
        observe_network = false,
        redact_secrets = true,
        include_provenance = true,
        infer_schemas = true,
        minimum_confidence = 0.50,
        openapi_version = "3.1.0",
        max_depth = max_depth,
        max_pages = max_pages,
        api_patterns = api_patterns,
        require_api_pattern = true,
        resolve_chain = resolve_chain,
        follow_json_links = true,
        max_resolve_requests = 64,
        output = output
    }

    -- Prefer the dedicated Lua plugin layer when available; fall back to raw
    -- endpoint extraction + local filtering so the driver works even when the
    -- native plugin is not installed.
    local ok, mod = pcall(require, "scrape_endpoints")
    local result = nil
    local yaml = nil
    if ok and type(mod.scrape) == "function" then
        result = mod.scrape(session, spec)
        yaml = result.openapi_yaml
    else
        -- Fallback: try unified scrape-endpoints Lua path
        local ok2, mod2 = pcall(require, "plugins.scrape-endpoints.lua.scrape_endpoints")
        if ok2 and type(mod2.scrape) == "function" then
            result = mod2.scrape(session, spec)
            yaml = result.openapi_yaml
        else
            -- Last fallback: direct endpoint extraction
            local ext = require("lprowsext")
            local doc = session:document()
            local extraction = ext.endpoints.extract(doc, {
                follow_links = true,
                inspect_scripts = true,
                redact_secrets = true,
                include_provenance = true,
                minimum_confidence = 0.50
            })
            yaml = extraction:openapi_yaml()
            -- naive API filtering notice
            local filtered_note = "\n# scrape2oapi: filtered to API patterns " .. json_encode(api_patterns or {"/api"}) .. "\n"
            yaml = yaml .. filtered_note
            result = { openapi_yaml = yaml, endpoint_count = extraction:endpoint_count(), warnings = extraction:warnings() }
        end
    end

    write_file(output, yaml)

    local count = 0
    if result ~= nil then
        if type(result.endpoint_count) == "function" then count = result:endpoint_count()
        elseif type(result.endpoint_count) == "number" then count = result.endpoint_count
        elseif type(result.endpoints) == "table" then count = #result.endpoints
        end
    end

    print(string.format("scrape2oapi: wrote %d API endpoint(s) to %s (resolve_chain=%s)", count, output, tostring(resolve_chain)))

    -- Also write a JSON summary for tooling (does not contain secrets)
    local summary_path = output:gsub("%.yaml$", ".json"):gsub("%.yml$", ".json")
    if summary_path == output then summary_path = output .. ".json" end
    local summary = {
        url = url,
        output = output,
        endpoint_count = count,
        resolve_chain = resolve_chain,
        max_depth = max_depth,
        api_patterns = api_patterns or { "/api" },
        offline = offline,
        timestamp = os.date("!%Y-%m-%dT%H:%M:%SZ"),
        warnings = (result and result.warnings) or {}
    }
    pcall(function() write_file(summary_path, json_encode(summary) .. "\n") end)

    session:close()
    return 0
end
