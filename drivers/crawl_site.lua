-- drivers/crawl_site.lua
-- The `crawl-site` driver: crawl a site and collect page metadata as JSONL.
--
-- Driver contract
-- ---------------
-- A ProwseTk driver is a Lua script loaded by `prowsetk run <name>` (or by a
-- LuaRuntime directly). It defines a global `main(args)` entrypoint where
-- `args` is a table of named arguments declared by the matching `[[drivers]]`
-- entry in `Prowse.toml`. `main` returns an integer exit code (0 = success);
-- fatal failures are raised with `error()` and propagate to the host.
--
-- Arguments (see README "Project Configuration"):
--   url     string  required        seed URL
--   depth   integer default 2       link-follow depth (0 = seed page only)
--   output  path    default "build/pages.jsonl"
--   html    string  optional        operate on this in-memory document instead
--                                   of the network (offline/deterministic runs)
--
-- Each JSONL line is one page: url, title, text_length, link_count, links,
-- script_count, form_count, meta, and an ISO-8601 timestamp.

local prowse = require("lprowse")

-- ---------------------------------------------------------------------------
-- Minimal JSON encoder (keeps the driver dependency-free).
-- ---------------------------------------------------------------------------

local function json_encode(value)
    local function encode(v)
        local kind = type(v)
        if kind == "nil" then
            return "null"
        elseif kind == "boolean" then
            return v and "true" or "false"
        elseif kind == "number" then
            if v ~= v or v == math.huge or v == -math.huge then
                return "null"
            end
            return string.format("%.14g", v)
        elseif kind == "string" then
            local escaped = v:gsub("\\", "\\\\")
            escaped = escaped:gsub('"', '\\"')
            escaped = escaped:gsub("\n", "\\n")
            escaped = escaped:gsub("\r", "\\r")
            escaped = escaped:gsub("\t", "\\t")
            escaped = escaped:gsub("[%c]", function(c)
                return string.format("\\u%04x", string.byte(c))
            end)
            return '"' .. escaped .. '"'
        elseif kind == "table" then
            local is_array = true
            local max_index = 0
            for key in pairs(v) do
                if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
                    is_array = false
                    break
                end
                if key > max_index then
                    max_index = key
                end
            end
            if is_array then
                local parts = {}
                for i = 1, max_index do
                    parts[i] = encode(v[i])
                end
                return "[" .. table.concat(parts, ",") .. "]"
            end
            local parts = {}
            for key, val in pairs(v) do
                parts[#parts + 1] = encode(tostring(key)) .. ":" .. encode(val)
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
        return "null"
    end
    return encode(value)
end

-- ---------------------------------------------------------------------------
-- Filesystem helpers.
-- ---------------------------------------------------------------------------

local function dirname(path)
    local normalized = tostring(path):gsub("\\", "/")
    local index = normalized:match(".*/()")
    if index ~= nil then
        return normalized:sub(1, index - 1)
    end
    return "."
end

local function shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function ensure_parent(path)
    local directory = dirname(path)
    if directory == "" or directory == "." then
        return true
    end
    local ok = os.execute("mkdir -p " .. shell_quote(directory))
    return ok == true
end

local function write_jsonl(path, records)
    ensure_parent(path)
    local file, err = io.open(path, "w")
    if file == nil then
        error("crawl_site: cannot open output file: " .. tostring(path) ..
              " (" .. tostring(err) .. ")", 2)
    end
    for _, record in ipairs(records) do
        file:write(json_encode(record), "\n")
    end
    file:close()
    return true
end

-- ---------------------------------------------------------------------------
-- URL helpers.
-- ---------------------------------------------------------------------------

local function origin_of(url)
    return tostring(url):match("^(https?://[^/]+)")
end

-- Resolves `href` against `base`. Returns nil for fragments and empty links.
local function resolve_url(base, href)
    if href == nil or href == "" then
        return nil
    end
    if href:sub(1, 1) == "#" then
        return nil
    end
    if href:match("^https?://") then
        return href
    end
    local origin = origin_of(base)
    if href:match("^//") then
        local scheme = tostring(base):match("^(https?:)")
        if scheme ~= nil and origin ~= nil then
            return scheme .. href
        end
        return nil
    end
    if origin == nil then
        return nil
    end
    if href:sub(1, 1) == "/" then
        return origin .. href
    end
    local directory = tostring(base):match("^(https?://.*/)[^/]*$")
    if directory == nil then
        return origin .. "/" .. href
    end
    return directory .. href
end

-- ---------------------------------------------------------------------------
-- Page extraction.
-- ---------------------------------------------------------------------------

local function extract_page(session, fallback_url)
    local document = session:document()
    if document == nil then
        error("crawl_site: session has no document", 2)
    end

    local links = {}
    for _, link in ipairs(document:links()) do
        local href = link:attribute("href")
        if href ~= nil and href ~= "" then
            links[#links + 1] = href
        end
    end

    local current = session:current_url()
    if current == nil or current == "" then
        current = fallback_url
    end

    local record = {
        url = current,
        title = document:title(),
        text_length = #document:text(),
        link_count = #links,
        links = links,
        script_count = #document:query_selector_all("script"),
        form_count = #document:query_selector_all("form"),
        timestamp = os.date("!%Y-%m-%dT%H:%M:%SZ")
    }

    local metadata = document:metadata()
    if metadata ~= nil and next(metadata) ~= nil then
        record.meta = metadata
    end
    return record
end

-- ---------------------------------------------------------------------------
-- Entrypoint.
-- ---------------------------------------------------------------------------

function main(args)
    args = args or {}
    local url = args.url or ""
    local depth = tonumber(args.depth) or 2
    local output = args.output or "build/pages.jsonl"
    local html = args.html
    local offline = type(html) == "string" and #html > 0

    if url == "" then
        error("crawl_site: missing required argument: url", 2)
    end
    if depth < 0 then
        depth = 0
    end

    local browser = prowse.browser.new({ javascript = false })
    local session = browser:create_session()

    if offline then
        session:load_html(html, url)
        local record = extract_page(session, url)
        record.offline = true
        write_jsonl(output, { record })
        print(string.format("crawl_site: wrote 1 page (%d links) to %s",
                            record.link_count, output))
        session:close()
        return 0
    end

    local visited = {}
    local results = {}
    local failures = {}
    local queue = { { url = url, depth = 0 } }
    local max_pages = 1000

    while #queue > 0 and #results < max_pages do
        local current = table.remove(queue, 1)
        if not visited[current.url] then
            visited[current.url] = true
            local ok, record = pcall(function()
                session:navigate(current.url)
                return extract_page(session, current.url)
            end)
            if ok then
                results[#results + 1] = record
                if current.depth < depth then
                    local origin = origin_of(url)
                    for _, href in ipairs(record.links) do
                        local resolved = resolve_url(url, href)
                        if resolved ~= nil and not visited[resolved] and
                           origin ~= nil and origin_of(resolved) == origin then
                            queue[#queue + 1] = {
                                url = resolved,
                                depth = current.depth + 1
                            }
                        end
                    end
                end
            else
                failures[#failures + 1] = {
                    url = current.url,
                    reason = tostring(record)
                }
            end
        end
    end

    write_jsonl(output, results)
    print(string.format("crawl_site: wrote %d page(s) to %s",
                        #results, output))
    if #failures > 0 then
        print(string.format("crawl_site: %d page(s) could not be fetched",
                            #failures))
    end
    session:close()
    return 0
end
