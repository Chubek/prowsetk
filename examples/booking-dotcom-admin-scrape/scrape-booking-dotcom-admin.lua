-- Booking.com admin recursive endpoint scraper.
--
-- Live runs read Booking.com credentials from dotenv before looking at the
-- process environment, authenticate through ezlogin, crawl same-origin admin
-- pages, and write OpenAPI plus Postman artifacts under
-- _scraped/booking-dotcom-admin by default. Offline runs pass args.html and do
-- not read credentials.

local source = debug.getinfo(1, "S").source
if source:sub(1, 1) == "@" then source = source:sub(2) end
local source_dir = source:match("^(.*)/[^/]*$") or "."
local repo_root = source_dir:match("^(.*)/examples/booking%-dotcom%-admin%-scrape$") or
                  source_dir .. "/../.."
package.path = repo_root .. "/?.lua;" .. repo_root .. "/?/init.lua;" .. package.path

local prowse = require("lprowse")

local DEFAULT_URL = "https://admin.booking.com/"
local DEFAULT_OUTPUT_DIR = "_scraped/booking-dotcom-admin"

local function require_first(...)
    local names = {...}
    local last_error = nil
    for _, name in ipairs(names) do
        local ok, module = pcall(require, name)
        if ok then return module end
        last_error = module
    end
    error("booking-dotcom-admin: required Lua module is unavailable: " .. tostring(names[1]) ..
          " (" .. tostring(last_error) .. ")", 2)
end

local ezlogin = require_first("ezlogin", "plugins.ezlogin.lua.ezlogin")
local scrape2oapi = require_first("scrape2oapi", "plugins.scrape2oapi.lua.scrape2oapi")
local scrape2postman = require_first("scrape2postman", "plugins.scrape2postman.lua.scrape2postman")

local function trim(value)
    return tostring(value or ""):match("^%s*(.-)%s*$")
end

local function dirname(path)
    local normalized = tostring(path):gsub("\\", "/")
    local index = normalized:match(".*/()")
    if index then return normalized:sub(1, index - 1) end
    return "."
end

local function shell_quote(value)
    return "'" .. tostring(value):gsub("'", "'\\''") .. "'"
end

local function ensure_dir(path)
    if not path or path == "" or path == "." then return true end
    local ok = os.execute("mkdir -p " .. shell_quote(path))
    if ok ~= true and ok ~= 0 then
        error("booking-dotcom-admin: cannot create output directory", 2)
    end
    return true
end

local function ensure_parent(path)
    return ensure_dir(dirname(path))
end

local function write_file(path, content)
    ensure_parent(path)
    local file, err = io.open(path, "wb")
    if not file then
        error("booking-dotcom-admin: cannot open output file: " .. tostring(path) ..
              " (" .. tostring(err) .. ")", 2)
    end
    file:write(content or "")
    file:close()
end

local function file_exists(path)
    local file = io.open(path, "rb")
    if file then file:close(); return true end
    return false
end

local function decode_dotenv_value(value, quote, line_number)
    if quote == "'" then
        local inner = value:match("^'(.*)'$")
        if inner == nil then
            error("load_dotenv: malformed quoted value at line " .. tostring(line_number), 2)
        end
        return inner
    end
    if quote == '"' then
        local inner = value:match('^"(.*)"$')
        if inner == nil then
            error("load_dotenv: malformed quoted value at line " .. tostring(line_number), 2)
        end
        return (inner:gsub("\\([nrt\\\"])", {
            n = "\n", r = "\r", t = "\t", ["\\"] = "\\", ['"'] = '"'
        }))
    end
    return trim((value:gsub("%s+#.*$", "")))
end

local function load_dotenv(path)
    local values = {}
    local filepath = path
    if not filepath or filepath == "" then
        if file_exists(".env") then
            filepath = ".env"
        elseif file_exists(".dotenv") then
            filepath = ".dotenv"
        else
            return values
        end
    end

    local file, err = io.open(filepath, "r")
    if not file then
        error("load_dotenv: cannot open dotenv file (" .. tostring(err) .. ")", 2)
    end

    local line_number = 0
    for line in file:lines() do
        line_number = line_number + 1
        local text = trim(line)
        if text ~= "" and not text:match("^#") then
            local key, raw = text:match("^export%s+([%w_]+)%s*=%s*(.*)$")
            if not key then key, raw = text:match("^([%w_]+)%s*=%s*(.*)$") end
            if key and raw ~= nil then
                local first = raw:sub(1, 1)
                values[key] = decode_dotenv_value(raw, (first == "'" or first == '"') and first or nil, line_number)
            else
                error("load_dotenv: malformed assignment at line " .. tostring(line_number), 2)
            end
        end
    end
    file:close()
    return values
end

local function env(name, dotenv)
    if dotenv and dotenv[name] ~= nil then return dotenv[name] end
    return os.getenv(name)
end

local function url_origin(url)
    return type(url) == "string" and url:match("^(https?://[^/?#]+)") or nil
end

local function url_host(url)
    return type(url) == "string" and url:match("^https?://([^/:?#]+)") or nil
end

local function resolve_url(base, ref)
    if type(ref) ~= "string" or ref == "" then return nil end
    if ref:match("^https?://") then return ref end
    local origin = url_origin(base)
    if not origin then return nil end
    if ref:sub(1, 2) == "//" then return origin:match("^(https?)") .. ":" .. ref end
    if ref:match("^[%w+.-]+:") then return nil end
    if ref:sub(1, 1) == "/" then return origin .. ref end
    local clean_base = base:gsub("[?#].*$", "")
    local dir = clean_base:match("^(.*/)[^/]*$") or (origin .. "/")
    local joined = dir .. ref
    local prefix = origin .. "/"
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

local function is_api_like(url)
    local path = (url or ""):match("^https?://[^/]+([^?#]*)") or url or ""
    local lower = path:lower()
    for _, marker in ipairs({
        "/api", "/v1", "/v2", "/v3", "/graphql", "/rest", "/rpc", "/json",
        "/data", "/internal", "/ajax", "/gateway", "/service", "/backend",
        "/bff", "/dml"
    }) do
        if lower:find(marker, 1, true) then return true end
    end
    return lower:match("%.json$") ~= nil
end

local function should_crawl_page(start_url, candidate)
    if not candidate or not same_origin(start_url, candidate) then return false end
    if is_api_like(candidate) then return false end
    local path = (candidate:match("^https?://[^/]+([^?#]*)") or ""):lower()
    if path:find("logout", 1, true) or path:find("signout", 1, true) or
       path:find("sign%-out") then
        return false
    end
    return true
end

local function booking_trusted(start_url, candidate)
    local start_host = url_host(start_url) or ""
    local host = url_host(candidate) or ""
    if host == "" then return false end
    if same_origin(start_url, candidate) then return true end
    if start_host:match("booking%.com$") then
        return host == "booking.com" or host:match("%.booking%.com$")
    end
    return false
end

local function response_header(response, name)
    for key, value in pairs((response or {}).headers or {}) do
        if key:lower() == name:lower() then return value end
    end
end

local function fetch_page(session, url)
    local response = session:request("GET", url)
    return response.body or "", response
end

local function load_page(session, url, body)
    session:load_html(body or "", url)
    return session:document()
end

local function authenticated(document, html, selector)
    if selector and selector ~= "" and document and type(document.query_selector) == "function" then
        local ok, element = pcall(function() return document:query_selector(selector) end)
        if ok and element ~= nil then return true end
    end
    local text = (html or ""):lower()
    return text:find("logout", 1, true) ~= nil or
           text:find("log out", 1, true) ~= nil or
           text:find("sign out", 1, true) ~= nil or
           text:find("account menu", 1, true) ~= nil
end

local function detect_blocked_login(html)
    local lower = (html or ""):lower()
    if lower:find("captcha", 1, true) or lower:find("human verification", 1, true) or
       lower:find("verification required", 1, true) then
        error("booking-dotcom-admin: login form not available; challenge detected", 2)
    end
    if lower:find("<noscript", 1, true) and not lower:find("<form", 1, true) then
        error("booking-dotcom-admin: login page requires browser JavaScript or captcha support", 2)
    end
end

local function extract_op_token(url, body)
    return (url or ""):match("[?&]op_token=([^&#]+)") or
           (body or ""):match('"op_token"%s*:%s*"([^"]+)"')
end

local function login(session, url, username, password, options)
    options = options or {}
    local first_body, first_response = fetch_page(session, url)
    local location = response_header(first_response, "location")
    local login_url = url
    if first_response.status and first_response.status >= 300 and first_response.status < 400 and location then
        local resolved = resolve_url(url, location)
        if not resolved or not booking_trusted(url, resolved) then
            error("booking-dotcom-admin: login redirect target is outside trusted origin", 2)
        end
        login_url = resolved
        first_body = select(1, fetch_page(session, login_url))
    end

    local op_token = extract_op_token(login_url, first_body)
    if op_token then
        local ok, err = pcall(function()
            ezlogin.configure(session, {
                mode = "oauth",
                login_url = login_url,
                username = username,
                password = password,
                op_token = op_token,
                as_token = options.as_token,
                trusted = function(candidate) return booking_trusted(url, candidate) end
            })
        end)
        if ok then return true end
        if tostring(err):find("human verification", 1, true) then
            error("booking-dotcom-admin: login form not available; challenge detected", 2)
        end
    end

    local ok, err = pcall(function()
        ezlogin.configure(session, {
            mode = "form",
            login_url = login_url,
            username = username,
            password = password,
            username_field = options.username_field or "username",
            password_field = options.password_field or "password",
            trusted = function(candidate) return booking_trusted(url, candidate) end
        })
    end)
    if not ok then
        local message = tostring(err)
        if message:find("requires JavaScript", 1, true) then
            error("booking-dotcom-admin: login page requires browser JavaScript or captcha support", 2)
        end
        if message:find("no login form", 1, true) then
            error("booking-dotcom-admin: login form not available", 2)
        end
        error("booking-dotcom-admin: login failed", 2)
    end
    return true
end

local function load_authenticated_page(session, url, success_selector)
    local body = select(1, fetch_page(session, url))
    local doc = load_page(session, url, body)
    if not authenticated(doc, body, success_selector) then
        error("booking-dotcom-admin: login was not confirmed", 2)
    end
    return body
end

local function endpoint_key(ep)
    return (ep.method or "get") .. "\0" .. (ep.url or ep.path or "")
end

local function redact_url(url)
    if type(url) ~= "string" then return url end
    return (url:gsub("([?&])([^&=#?]*)(=)([^&#]*)", function(prefix, key, equals, value)
        local lower = key:lower()
        if lower:find("token", 1, true) or lower:find("secret", 1, true) or
           lower:find("password", 1, true) or lower:find("key", 1, true) or
           lower:find("auth", 1, true) then
            return prefix .. key .. equals .. "[REDACTED]"
        end
        return prefix .. key .. equals .. value
    end))
end

local function add_endpoint(target, seen, ep, authenticated_flag)
    if type(ep) ~= "table" then return end
    if not ep.url or ep.url == "" then return end
    ep.authenticated = authenticated_flag == true
    local key = endpoint_key(ep)
    if not seen[key] then
        seen[key] = true
        target[#target + 1] = ep
    end
end

local function redacted_endpoints(endpoints)
    local out = {}
    for _, ep in ipairs(endpoints or {}) do
        local copy = {}
        for key, value in pairs(ep) do copy[key] = value end
        copy.url = redact_url(copy.url)
        copy.source = redact_url(copy.source)
        copy.final_url = redact_url(copy.final_url)
        out[#out + 1] = copy
    end
    return out
end

local function path_from_url(url)
    return (url or ""):match("^https?://[^/]+([^?#]*)") or url or ""
end

local function make_endpoint(url, source, method)
    return {
        url = url,
        path = path_from_url(url),
        method = method or "get",
        source = source,
        discovery_method = "script-or-json",
        confidence = 0.60,
        parameters = {},
        notes = {"heuristically discovered by Booking.com admin driver"}
    }
end

local function scan_urls(text, base_url)
    local found, seen = {}, {}
    local function add(candidate)
        local resolved = resolve_url(base_url, candidate)
        if resolved and same_origin(base_url, resolved) and not seen[resolved] then
            seen[resolved] = true
            found[#found + 1] = resolved
        end
    end
    text = text or ""
    for quoted in text:gmatch('"([^"]+)"') do add(quoted) end
    for quoted in text:gmatch("'([^']+)'") do add(quoted) end
    for url in text:gmatch("https?://[%w%._~:/%?#%[%]@!$&%(%)%*%+,;=%%%-]+") do add(url) end
    for path in text:gmatch("/[%w%._~/%?#%[%]@!$&%(%)%*%+,;=%%%-]+") do add(path) end
    return found
end

local function inspect_scripts(session, document, page_url, endpoints, seen_endpoints, authenticated_flag)
    if not document or type(document.query_selector_all) ~= "function" then return end
    local fetched_scripts = 0
    local max_scripts = 32
    local queue, seen_scripts = {}, {}
    for _, script in ipairs(document:query_selector_all("script[src]")) do
        local src = resolve_url(page_url, script:attribute("src"))
        if src and same_origin(page_url, src) and not seen_scripts[src] then
            seen_scripts[src] = true
            queue[#queue + 1] = src
        end
    end
    while #queue > 0 and fetched_scripts < max_scripts do
        local script_url = table.remove(queue, 1)
        fetched_scripts = fetched_scripts + 1
        local ok, body = pcall(function() return select(1, fetch_page(session, script_url)) end)
        if ok then
            for import_ref in body:gmatch("import%s*%(%s*['\"]([^'\"]+)['\"]%s*%)") do
                local imported = resolve_url(script_url, import_ref)
                if imported and same_origin(page_url, imported) and not seen_scripts[imported] then
                    seen_scripts[imported] = true
                    queue[#queue + 1] = imported
                end
            end
            for _, url in ipairs(scan_urls(body, script_url)) do
                if is_api_like(url) then
                    add_endpoint(endpoints, seen_endpoints, make_endpoint(url, script_url), authenticated_flag)
                elseif path_from_url(url):match("%.js$") and not seen_scripts[url] and fetched_scripts < max_scripts then
                    seen_scripts[url] = true
                    queue[#queue + 1] = url
                end
            end
        end
    end
end

local function resolve_api_chains(session, endpoints, seen_endpoints, start_url, args, authenticated_flag)
    local max_depth = tonumber(args.max_api_depth) or 2
    local max_requests = tonumber(args.max_api_requests) or 64
    local queue, visited = {}, {}
    for _, ep in ipairs(endpoints) do
        if is_api_like(ep.url) then queue[#queue + 1] = {url = ep.url, depth = 0} end
    end
    local count = 0
    while #queue > 0 and count < max_requests do
        local item = table.remove(queue, 1)
        if not visited[item.url] and same_origin(start_url, item.url) then
            visited[item.url] = true
            count = count + 1
            local ok, body = pcall(function() return select(1, fetch_page(session, item.url)) end)
            if ok and item.depth < max_depth then
                for _, url in ipairs(scan_urls(body, item.url)) do
                    if is_api_like(url) and same_origin(start_url, url) then
                        add_endpoint(endpoints, seen_endpoints, make_endpoint(url, item.url), authenticated_flag)
                        if not visited[url] then
                            queue[#queue + 1] = {url = url, depth = item.depth + 1}
                        end
                    end
                end
            end
        end
    end
end

local function discover(session, url, spec)
    local result = scrape2oapi.scrape(session, spec)
    return result.endpoints or {}
end

local function crawl(session, start_url, args, authenticated_flag)
    local max_depth = tonumber(args.max_depth) or 2
    local max_pages = tonumber(args.max_pages) or 100
    local max_api_depth = tonumber(args.max_api_depth) or 2
    local max_api_requests = tonumber(args.max_api_requests) or 64
    local endpoints, seen_endpoints = {}, {}
    local visited, queue = {}, {{url = start_url, depth = 0}}
    local pages = 0

    while #queue > 0 and pages < max_pages do
        local item = table.remove(queue, 1)
        if not visited[item.url] and should_crawl_page(start_url, item.url) then
            visited[item.url] = true
            pages = pages + 1
            local body = select(1, fetch_page(session, item.url))
            local document = load_page(session, item.url, body)
            inspect_scripts(session, document, item.url, endpoints, seen_endpoints, authenticated_flag)
            local page_spec = {
                url = item.url,
                follow_links = true,
                inspect_scripts = true,
                scrape_all_paths = true,
                observe_network = false,
                redact_secrets = true,
                include_provenance = true,
                infer_schemas = true,
                minimum_confidence = 0.45,
                openapi_version = "3.1.0",
                require_api_pattern = false,
                resolve_chain = false,
                follow_json_links = false,
                max_depth = max_api_depth,
                max_pages = max_pages,
                max_resolve_requests = max_api_requests
            }
            for _, ep in ipairs(discover(session, item.url, page_spec)) do
                add_endpoint(endpoints, seen_endpoints, ep, authenticated_flag)
            end

            local api_spec = {
                url = item.url,
                follow_links = true,
                inspect_scripts = true,
                scrape_all_paths = false,
                observe_network = false,
                redact_secrets = true,
                include_provenance = true,
                infer_schemas = true,
                minimum_confidence = 0.45,
                openapi_version = "3.1.0",
                require_api_pattern = true,
                resolve_chain = true,
                follow_json_links = true,
                max_depth = max_api_depth,
                max_pages = max_pages,
                max_resolve_requests = max_api_requests
            }
            for _, ep in ipairs(discover(session, item.url, api_spec)) do
                add_endpoint(endpoints, seen_endpoints, ep, authenticated_flag)
            end
            if item.depth < max_depth and document and type(document.query_selector_all) == "function" then
                for _, link in ipairs(document:query_selector_all("a[href]")) do
                    local href = link:attribute("href")
                    local next_url = resolve_url(item.url, href)
                    if next_url and should_crawl_page(start_url, next_url) and not visited[next_url] then
                        queue[#queue + 1] = {url = next_url, depth = item.depth + 1}
                    end
                end
            end
        end
    end
    resolve_api_chains(session, endpoints, seen_endpoints, start_url, args, authenticated_flag)
    return endpoints, pages
end

local function output_paths(args)
    local output_dir = args.output_dir or args.directory or DEFAULT_OUTPUT_DIR
    ensure_dir(output_dir)
    local output = args.output or (output_dir .. "/BookingDotcomAdminPanel.yaml")
    local postman = args.postman or (output_dir .. "/BookingDotcomAdminPanel.postman_collection.json")
    return output, postman
end

local function append_metadata(yaml, authenticated_flag)
    return yaml ..
        "x-prowsetk-booking-admin:\n" ..
        "  authenticated: " .. tostring(authenticated_flag == true) .. "\n" ..
        "  complete: false\n" ..
        "  note: 'Heuristic endpoint discovery; not authoritative API documentation.'\n"
end

function main(args)
    args = args or {}
    local url = args.url or DEFAULT_URL
    local output, postman = output_paths(args)
    local html = args.html
    local offline = type(html) == "string" and html ~= ""

    local browser = prowse.browser.new({
        javascript = true,
        follow_redirects = false,
        observe_network = false
    })
    local session = browser:create_session()
    local ok, err = pcall(function()
        local endpoints = nil
        local authenticated_flag = false
        if offline then
            session:load_html(html, url)
            endpoints = discover(session, url, {
                url = url,
                html = html,
                follow_links = true,
                inspect_scripts = true,
                scrape_all_paths = true,
                observe_network = false,
                redact_secrets = true,
                include_provenance = true,
                infer_schemas = true,
                require_api_pattern = false,
                resolve_chain = false,
                max_depth = tonumber(args.max_api_depth) or 2,
                max_pages = tonumber(args.max_pages) or 100
            })
        else
            local dotenv = load_dotenv(args.dotenv)
            local username = env("BOOKING_DOTCOM_USER", dotenv)
            local password = env("BOOKING_DOTCOM_PASS", dotenv)
            if not username or username == "" or not password or password == "" then
                error("booking-dotcom-admin: missing Booking.com credentials", 2)
            end
            login(session, url, username, password, {
                as_token = env("BOOKING_DOTCOM_AS_TOKEN", dotenv),
                success_selector = args.success_selector
            })
            load_authenticated_page(session, url, args.success_selector)
            authenticated_flag = true
            endpoints = crawl(session, url, args, true)
        end

        local safe_endpoints = redacted_endpoints(endpoints or {})
        local yaml = scrape2oapi.render_openapi_yaml(safe_endpoints, {
            openapi_version = "3.1.0",
            redact_secrets = true,
            include_provenance = true,
            infer_schemas = true
        })
        yaml = append_metadata(yaml, authenticated_flag)
        write_file(output, yaml)

        local postman_json = scrape2postman.render_postman_json(safe_endpoints, {
            collection_name = "Discovered API (Booking.com Admin)",
            redact_secrets = true,
            include_provenance = true
        })
        write_file(postman, postman_json)

        print("booking-dotcom-admin: wrote heuristic OpenAPI and Postman outputs")
    end)

    pcall(function() session:close() end)
    if not ok then error(err, 0) end
    return 0
end
