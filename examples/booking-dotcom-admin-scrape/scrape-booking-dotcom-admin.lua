-- Booking.com admin recursive endpoint scraper.
--
-- Live runs read Booking.com credentials from dotenv before looking at the
-- process environment, authenticate through ezlogin, crawl same-origin admin
-- pages, resolve a RESTful surface through restful-resolver, enrich
-- request/response schemas and URL parameters through schema-grabber, and
-- write OpenAPI plus Postman artifacts under
-- _scraped/booking-dotcom-admin by default. Offline runs pass args.html and do
-- not read credentials. Pass --no-xcors (boolean, default false) to drop any
-- discovered endpoint whose host is outside the booking.com TLD
-- (booking.com or *.booking.com).
--
-- Beacon network results (Firefox oracle, heuristic): pass args.beacon_json
-- with a network_info flash_data document (hermetic, works offline and in
-- tests) or pass --flash-on true for a live beacond Flash (optionally with
-- args.beacon_socket, default /tmp/beacond.sock). Observed
-- Firefox requests merge as beacon-network endpoints before restful
-- resolution and schema enrichment. Live collection registers a Flash with
-- beaconctl, waits for the user-consented addon flow (List Flashes, Connect
-- to Flash, Send Network Info), then polls the bounded queue; any failure
-- keeps the seed endpoints with a status note.
--
-- OpenCode endpoint cleanup (opt-in): pass args.opencode_json with a pre-baked
-- agent answer (hermetic, works offline and in tests) or enable a live call
-- with args.opencode (or args.opencode_enabled) through the lopencode native
-- module. The agent may only remove endpoints; invented URLs are ignored and
-- original tables are kept verbatim. Results are recorded in the
-- x-prowsetk-opencode metadata block.

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
local scrape_endpoints = require_first("scrape_endpoints", "plugins.scrape-endpoints.lua.scrape_endpoints")
local captcha_handler = require_first("captcha_handler", "plugins.captcha-handler.lua.captcha_handler")
local restful_resolver = nil
do
    local ok, module = pcall(require_first,
        "restful_resolver", "plugins.restful-resolver.lua.restful_resolver")
    if ok then restful_resolver = module end
end
local schema_grabber = nil
do
    local ok, module = pcall(require_first,
        "schema_grabber", "plugins.schema-grabber.lua.schema_grabber")
    if ok then schema_grabber = module end
end
local beacon_spec = nil
do
    local ok, module = pcall(require_first,
        "beacon", "plugins.beacon.lua.beacon")
    if ok then beacon_spec = module end
end
local ebpf_interface = nil
do
    local ok, module = pcall(require_first,
        "ebpf_interface", "plugins.ebpf-interface.lua.ebpf_interface")
    if ok then ebpf_interface = module end
end

local function trim(value)
    return tostring(value or ""):match("^%s*(.-)%s*$")
end

local verbose = false
local function verbose_requested(args)
    return args.verbose == true or tostring(args.verbose or ""):lower() == "true"
end
local function debug_log(message)
    if verbose then io.stderr:write("booking-dotcom-admin: " .. message .. "\n") end
end
-- Diagnostics expose hosts and evidence flags, never URL queries, page text,
-- cookie values, credentials or raw transport/JavaScript errors.
local function diagnostic_host(url)
    local authority = tostring(url or ""):match("^https?://([^/?#]+)") or ""
    local host = authority:gsub("^.*@", "")
    if #host > 255 or not host:match("^[%w.:%[%]%-]+$") then return "unavailable" end
    return host
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

-- Login can fail before scrape_endpoints gets a chance to offer its handoff. Keep
-- this small driver-level bridge so the configured assistant browser is still
-- offered for MFA, human verification, or site-specific browser checks.
local function offer_login_assistant_browser(session, url, args, reason)
    args = args or {}
    local enabled = args.assistant_browser_force == true or
        tostring(args.assistant_browser_force or ""):lower() == "true" or
        args.assistant_browser_enabled == true or
        tostring(args.assistant_browser_enabled or ""):lower() == "true"
    if not enabled then return false end

    local command = os.getenv("PROWSETK_ASSISTANT_BROWSER") or
        args.assistant_browser or "assistant-browser"
    if command == "" then command = "assistant-browser" end
    local method = args.assistant_browser_method or "manual"
    local forced = args.assistant_browser_force == true or
        tostring(args.assistant_browser_force or ""):lower() == "true"
    local cookie_path = args.cookies_json
    local cookie_hint = cookie_path and
        (" Export fresh cookies to " .. tostring(cookie_path) .. " before pressing Enter.") or
        " Export fresh browser cookies and rerun with --cookies-json FILE before pressing Enter."
    if not forced then
        io.stderr:write("booking-dotcom-admin: " .. tostring(reason) ..
            ". Launch assistant browser with " .. tostring(method) ..
            " handoff? [y/N]." .. cookie_hint .. "\n")
        io.stderr:write("booking-dotcom-admin: continue? [y/N] ")
        local answer = io.read("*l") or ""
        answer = trim(answer):lower()
        if answer ~= "y" and answer ~= "yes" then return false end
    else
        io.stderr:write("booking-dotcom-admin: " .. tostring(reason) .. "." ..
            cookie_hint .. "\n")
    end

    io.stderr:write("booking-dotcom-admin: launching assistant browser for " .. diagnostic_host(url) .. "\n")
    -- Firefox can remain open for the whole crawl. Disconnect its terminal
    -- streams and ignore hangup so os.execute waits only for the launcher.
    os.execute("nohup " .. tostring(command) .. " " .. shell_quote(url) ..
        " </dev/null >/dev/null 2>&1 &")
    io.stderr:write("booking-dotcom-admin: press Enter after browser interaction is complete. " ..
        "Leave the browser open and press Enter in this terminal. ")
    io.stderr:flush()
    if io.read("*l") == nil then
        error("booking-dotcom-admin: browser interaction was not confirmed", 2)
    end
    io.stderr:write("\n")
    local cookie_command = os.getenv("PROWSETK_ASSISTANT_BROWSER_COOKIE_COMMAND") or
        args.assistant_browser_cookie_command or ""
    local default_grabber = cookie_command == ""
    if cookie_command == "" then
        cookie_command = shell_quote(repo_root .. "/scripts/grab-firefox-cookies.sh")
    end
    if cookie_command ~= "" and cookie_path and cookie_path ~= "" then
        io.stderr:write("booking-dotcom-admin: grabbing Firefox cookies\n")
        local ok, _, status = os.execute(tostring(cookie_command) .. " " .. shell_quote(cookie_path) ..
            (default_grabber and verbose and " --verbose" or "") ..
            " >/dev/null" .. (default_grabber and "" or " 2>/dev/null"))
        if ok ~= true and ok ~= 0 then
            error("booking-dotcom-admin: Firefox cookie capture failed (exit " ..
                tostring(tonumber(status) or 1) ..
                "); check FIREFOX_PROFILE_DIR or the configured cookie command", 2)
        end
    end
    local imported = 0
    if cookie_path and cookie_path ~= "" and session and
       type(session.import_cookies_json) == "function" then
        local ok, count = pcall(function()
            return session:import_cookies_json(cookie_path)
        end)
        if not ok then
            error("booking-dotcom-admin: Firefox cookie import failed; check the cookie JSON file", 2)
        end
        imported = tonumber(count) or 0
        io.stderr:write("booking-dotcom-admin: imported " .. tostring(imported) .. " cookies (values omitted)\n")
        if imported == 0 then
            error("booking-dotcom-admin: Firefox cookie import returned no cookies; check the active Firefox profile", 2)
        end
    else
        error("booking-dotcom-admin: Firefox cookie import requires a cookie file and managed session", 2)
    end
    return true, imported
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
        "/bff", "/dml", "/hotel/hoteladmin", "/partner-settings",
        "/telemetry", "challenge", "/beacon", "/collect"
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
    if type(candidate) ~= "string" or not candidate:match("^https://") then return false end
    local start_host = (url_host(start_url) or ""):lower()
    local host = (url_host(candidate) or ""):lower()
    if host == "" then return false end
    if same_origin(start_url, candidate) then return true end
    if start_host == "booking.com" or start_host:match("%.booking%.com$") then
        return host == "booking.com" or host:match("%.booking%.com$") ~= nil
    end
    return false
end

-- `--no-xcors` drops endpoints outside the booking.com TLD. The CLI maps
-- `--no-xcors VALUE` onto the `no-xcors` driver argument (coerced to a Lua
-- boolean); direct `main(args)` callers may use `["no-xcors"]` or `no_xcors`
-- with a boolean or a truthy string ("true"/"1"/"yes"/"on").
local no_xcors_only = false

local function is_booking_tld_host(host)
    host = (host or ""):lower()
    if host == "" then return false end
    if host == "booking.com" then return true end
    return host:match("%.booking%.com$") ~= nil
end

local function is_booking_tld_url(url)
    if type(url) ~= "string" or url == "" then return false end
    if url:match("^https?://") then
        return is_booking_tld_host(url_host(url))
    end
    if url:sub(1, 1) == "/" then return true end
    if url:match("^[%w+.-]+:") then return false end
    return true
end

local function no_xcors_requested(args)
    args = args or {}
    local raw = args["no-xcors"]
    if raw == nil then raw = args.no_xcors end
    if raw == true then return true end
    if type(raw) == "number" then return raw ~= 0 end
    if type(raw) == "string" then
        local lower = raw:lower()
        return lower == "true" or lower == "1" or lower == "yes" or lower == "on"
    end
    return false
end

-- `--api-only` drops garbage/gunk endpoints (static assets, bundles, plain
-- pages) that cannot serve as proper API endpoints. The CLI maps
-- `--api-only VALUE` onto the `api-only` driver argument; direct `main(args)`
-- callers may use `["api-only"]` or `api_only`. On by default: only an
-- explicit false (boolean, 0, or "false"/"0"/"no"/"off") disables it.
local function api_only_requested(args)
    args = args or {}
    local raw = args["api-only"]
    if raw == nil then raw = args.api_only end
    if raw == nil then return true end
    if raw == true then return true end
    if raw == false then return false end
    if type(raw) == "number" then return raw ~= 0 end
    if type(raw) == "string" then
        local lower = raw:lower()
        if lower == "false" or lower == "0" or lower == "no" or lower == "off" then
            return false
        end
        return true
    end
    return true
end

-- Hosts (besides the page origin itself) whose `<script src>` bundles may be
-- fetched for static endpoint scanning. The extranet app serves its real
-- client code from Booking's static CDN, so same-origin-only fetching would
-- ignore nearly every bundle. Entries match exactly or as a parent domain
-- ("bstatic.com" covers "r-xx.bstatic.com"). Fetched bytes are only scanned
-- as text, never executed; session cookies stay domain-scoped by the jar, so
-- page cookies are not sent to these hosts.
local function extra_script_hosts(args)
    args = args or {}
    local raw = args.script_origins
    if raw == nil or raw == "" then raw = "bstatic.com" end
    local hosts = {}
    for entry in tostring(raw):gmatch("[^,%s]+") do
        hosts[#hosts + 1] = entry:lower()
    end
    return hosts
end

local function host_matches_entry(host, entry)
    if host == entry then return true end
    return host:sub(-(#entry + 1)) == "." .. entry
end

local function script_fetch_allowed(page_url, src, args)
    if same_origin(page_url, src) then return true end
    local host = (url_host(src) or ""):lower()
    if host == "" then return false end
    for _, entry in ipairs(extra_script_hosts(args)) do
        if host_matches_entry(host, entry) then return true end
    end
    return false
end

local function filter_booking_tld(endpoints)
    local out = {}
    for _, ep in ipairs(endpoints or {}) do
        if type(ep) == "table" and is_booking_tld_url(ep.url) then
            out[#out + 1] = ep
        end
    end
    return out
end

local function response_header(response, name)
    for key, value in pairs((response or {}).headers or {}) do
        if key:lower() == name:lower() then return value end
    end
end

local function is_redirect(status)
    return status == 301 or status == 302 or status == 303 or status == 307 or status == 308
end

local function fetch_page(session, url, options)
    options = options or {}
    local method = options.method or "GET"
    local current_url = url
    local redirects = {}
    local max_redirects = tonumber(options.max_redirects) or 0
    for _ = 0, max_redirects do
        local response = session:request(method, current_url, {body = options.body, headers = options.headers})
        debug_log("login request: " .. method .. " host=" .. diagnostic_host(current_url) ..
            " HTTP=" .. tostring(response.status) .. " bytes=" .. tostring(#(response.body or "")))
        response.final_url = current_url
        response.redirect_chain = redirects
        response.captcha = captcha_handler.inspect_response(method, current_url, response)
        local location = response_header(response, "Location")
        if not options.follow_redirects or not is_redirect(response.status) or not location then
            return response.body or "", response
        end
        local next_url = resolve_url(current_url, location)
        debug_log("login redirect: next-host=" .. diagnostic_host(next_url))
        if not next_url then
            return response.body or "", response
        end
        local trust_base = options.trusted_start or current_url
        if options.trusted_start and not booking_trusted(trust_base, next_url) then
            error("booking-dotcom-admin: redirect target is outside trusted HTTPS origin", 2)
        end
        redirects[#redirects + 1] = current_url
        current_url = next_url
        if response.status == 303 or ((response.status == 301 or response.status == 302) and method ~= "GET" and method ~= "HEAD") then
            method = "GET"
            options.body = nil
        end
    end
    error("booking-dotcom-admin: exceeded " .. tostring(max_redirects) .. " login redirects", 2)
end

local function load_page(session, url, body)
    session:load_html(body or "", url)
    return session:document()
end

local function authenticated(document, html, selector)
    if not document then return false end
    if document:query_selector('input[type="password"]') then return false end
    if selector and selector ~= "" and document and type(document.query_selector) == "function" then
        local ok, element = pcall(function() return document:query_selector(selector) end)
        if ok and element ~= nil then return true end
    end
    for _, element in ipairs(document:query_selector_all("a[href], button")) do
        local href = (element:attribute("href") or ""):lower():gsub("[?#].*$", "")
        local label = trim(element:text()):lower()
        if href:find("logout", 1, true) or href:find("signout", 1, true) or
           href:find("sign-out", 1, true) or label == "log out" or
           label == "sign out" or label == "logout" then return true end
    end
    return document:query_selector('[data-testid="account-menu"], [aria-label="Account menu"]') ~= nil
end

local function beacon_authenticated(document, beacon, beacon_type)
    if not document or type(beacon) ~= "string" or beacon == "" then return false end
    beacon_type = (beacon_type or "auto"):lower()
    local value = beacon
    local prefix, payload = beacon:match("^(%w+)%s*=%s*(.+)$")
    if prefix then beacon_type, value = prefix:lower(), payload end
    if beacon_type == "auto" then
        beacon_type = value:match("^//") and "xpath" or "css"
    end
    if beacon_type == "xpath" and type(document.xpath) == "function" then
        local ok, nodes = pcall(function() return document:xpath(value) end)
        return ok and type(nodes) == "table" and #nodes > 0, not ok
    elseif beacon_type == "css" and type(document.query_selector) == "function" then
        local ok, node = pcall(function() return document:query_selector(value) end)
        return ok and node ~= nil, not ok
    elseif beacon_type == "text" and type(document.text) == "function" then
        local ok, text = pcall(function() return document:text() end)
        return ok and tostring(text or ""):lower():find(value:lower(), 1, true) ~= nil, not ok
    end
    return false, true
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

local function confirmed_challenge(response)
    local challenge = response and response.captcha
    return challenge and challenge.activated and challenge.server_confirmed
end

local function challenge_status(response)
    local challenge = response and response.captcha
    if not challenge or not challenge.activated then return "none" end
    return (challenge.server_confirmed and "confirmed" or "heuristic") ..
        ":" .. tostring(challenge.category or "anti-bot")
end

local function extract_op_token(url, body)
    return (url or ""):match("[?&]op_token=([^&#]+)") or
           (body or ""):match('"op_token"%s*:%s*"([^"]+)"')
end

local function json_unescape_ascii(value)
    if type(value) ~= "string" then return nil end
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

local function extract_json_string(text, name)
    text = text or ""
    local key_start, key_end = text:find('"' .. name .. '"', 1, true)
    if not key_end then return nil end
    local colon = text:find(":", key_end + 1, true)
    if not colon then return nil end
    local quote = text:find('"', colon + 1, true)
    if not quote then return nil end
    local out, escaped = {}, false
    local index = quote + 1
    while index <= #text do
        local ch = text:sub(index, index)
        if escaped then
            out[#out + 1] = "\\" .. ch
            escaped = false
        elseif ch == "\\" then
            escaped = true
        elseif ch == '"' then
            return json_unescape_ascii(table.concat(out))
        else
            out[#out + 1] = ch
        end
        index = index + 1
    end
    return nil
end

local function login(session, url, username, password, options)
    options = options or {}
    local first_body, first_response = options.initial_body, options.initial_response
    if not first_response then first_body, first_response = fetch_page(session, url, {
        follow_redirects = true,
        max_redirects = 8,
        trusted_start = url
    }) end
    local location = response_header(first_response, "location")
    local login_url = url
    if first_response.final_url and first_response.final_url ~= "" and
       booking_trusted(url, first_response.final_url) then
        login_url = first_response.final_url
    end
    if first_response.status and first_response.status >= 300 and first_response.status < 400 and location then
        local resolved = resolve_url(url, location)
        if not resolved or not booking_trusted(url, resolved) then
            error("booking-dotcom-admin: login redirect target is outside trusted origin", 2)
        end
        login_url = resolved
        first_body = select(1, fetch_page(session, login_url))
    end
    if confirmed_challenge(first_response) then
        error("booking-dotcom-admin: login form not available; challenge detected (" ..
              tostring(first_response.captcha.diagnostic) .. ")", 2)
    end

    local op_token = extract_op_token(login_url, first_body)
    local page_as_token = extract_json_string(first_body, "as_token")
    local login_diagnostics = string.format(
        "status=%s final_url=%s login_host=%s login_has_query=%s body_len=%d body_op_token=%s body_as_token=%s noscript=%s challenge=%s",
        tostring(first_response.status),
        tostring(first_response.final_url ~= nil and first_response.final_url ~= ""),
        tostring(url_host(login_url) or ""),
        tostring(type(login_url) == "string" and login_url:find("?", 1, true) ~= nil),
        #(first_body or ""),
        tostring(extract_json_string(first_body, "op_token") ~= nil),
        tostring(page_as_token ~= nil),
        tostring((first_body or ""):lower():find("<noscript", 1, true) ~= nil),
        challenge_status(first_response))
    if op_token then
        local ok, err = pcall(function()
            ezlogin.configure(session, {
                mode = "oauth",
                login_url = login_url,
                username = username,
                password = password,
                op_token = op_token,
                as_token = options.as_token or page_as_token,
                trusted = function(candidate) return booking_trusted(url, candidate) end
            })
        end)
        if ok then return true end
        if tostring(err):find("human verification", 1, true) then
            error("booking-dotcom-admin: login form not available; challenge detected", 2)
        end
        error("booking-dotcom-admin: oauth login failed: " .. tostring(err):gsub("[\r\n].*$", ""), 2)
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
            error("booking-dotcom-admin: login page requires browser JavaScript or captcha support (" ..
                  login_diagnostics .. ")", 2)
        end
        if message:find("no login form", 1, true) then
            error("booking-dotcom-admin: login form not available", 2)
        end
        error("booking-dotcom-admin: login failed", 2)
    end
    return true
end

local function apply_captcha_inputs(session, args, dotenv)
    local clearance_cookie = args.clearance_cookie
    if not clearance_cookie or clearance_cookie == "" then
        clearance_cookie = env("BOOKING_DOTCOM_CLEARANCE_COOKIE", dotenv)
    end
    if clearance_cookie and clearance_cookie ~= "" then
        session:set_header("Cookie", clearance_cookie)
    end

    local captcha_token = args.captcha_token
    if not captcha_token or captcha_token == "" then
        captcha_token = env("BOOKING_DOTCOM_CAPTCHA_TOKEN", dotenv)
    end
    return {
        clearance_cookie = clearance_cookie,
        captcha_token = captcha_token
    }
end

local function captcha_handler_note(inputs, response)
    local challenge = response and response.captcha or {activated = true, category = "captcha"}
    local method = captcha_handler.select_handling_method(challenge, {
        has_clearance_cookie = inputs and inputs.clearance_cookie and inputs.clearance_cookie ~= "",
        has_pre_solved_token = inputs and inputs.captcha_token and inputs.captcha_token ~= ""
    }, {
        allow_cookie_session_reuse = true,
        allow_pre_solved_token = true,
        allow_wait_for_clearance = true
    })
    if method == "cookie-session-reuse" then
        return "captcha-handler cookie-session-reuse did not clear the challenge"
    end
    if method == "pre-solved-token" then
        return "captcha-handler pre-solved-token is configured but cannot be safely injected into this challenge"
    end
    local evidence = {}
    for _, signal in ipairs(challenge.signals or {}) do
        evidence[#evidence + 1] = signal.name
    end
    local detail = response and (" (HTTP " .. tostring(response.status) ..
        "; signals=" .. table.concat(evidence, ",") .. ")") or ""
    return "captcha-handler detected a confirmed challenge" .. detail ..
        "; complete verification in your browser, export fresh session cookies, and retry with --cookies-json FILE"
end

local function probe_authenticated_page(session, url, success_selector, success_beacon, success_beacon_type)
    local body, response = fetch_page(session, url, {
        follow_redirects = true, max_redirects = 8, trusted_start = url
    })
    local challenge = confirmed_challenge(response)
    local doc = not challenge and load_page(session, response.final_url, body) or nil
    local marker = authenticated(doc, body, success_selector)
    local beacon, beacon_error = beacon_authenticated(doc, success_beacon, success_beacon_type)
    local origin_ok = same_origin(url, response.final_url)
    local valid = not challenge and response.status >= 200 and response.status < 300 and
        origin_ok and (marker or beacon)
    if verbose or not valid then
        io.stderr:write("booking-dotcom-admin: login confirmation: HTTP=" .. tostring(response.status) ..
            " redirects=" .. tostring(#response.redirect_chain) ..
            " final-host=" .. diagnostic_host(response.final_url) ..
            " same-origin=" .. tostring(origin_ok) .. " auth-marker=" .. tostring(marker) ..
            " beacon-configured=" .. tostring(type(success_beacon) == "string" and success_beacon ~= "") ..
            " beacon-match=" .. tostring(beacon) .. " beacon-error=" .. tostring(beacon_error == true) ..
            " password-field=" .. tostring(doc ~= nil and doc:query_selector('input[type="password"]') ~= nil) ..
            " challenge=" .. challenge_status(response) .. " bytes=" .. tostring(#body) ..
            " confirmed=" .. tostring(valid) .. "\n")
    end
    return valid, body, response
end

local function load_authenticated_page(session, url, success_selector, success_beacon, success_beacon_type)
    local valid, body, response = probe_authenticated_page(
        session, url, success_selector, success_beacon, success_beacon_type)
    if confirmed_challenge(response) then
        error("booking-dotcom-admin: login form not available; challenge detected", 2)
    end
    if not valid then
        error("booking-dotcom-admin: login was not confirmed (HTTP " ..
              tostring(response.status) .. ", redirects=" ..
              tostring(#response.redirect_chain) ..
              "); session may have expired or require interactive verification", 2)
    end
    return response.final_url
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
    if no_xcors_only and not is_booking_tld_url(ep.url) then return end
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

-- Beacon network Flash results (Firefox oracle, heuristic).
--
-- The addon sends `network_info` flash_data only after an explicit user
-- click, and only from the connected HTTP(S) tab. Each entry carries
-- bounded metadata — method, origin+path URL (no query or body), and
-- resource type — captured after connect; earlier requests are unavailable.
-- This driver never treats Beacon data as authoritative: entries become
-- ordinary heuristic endpoints with beacon-network provenance and flow
-- through the same redaction, --no-xcors, api-only, restful, and schema
-- stages as every other discovery.
local function beacon_arg(args, name)
    args = args or {}
    local value = args[name]
    if value == nil then value = args[name:gsub("_", "-")] end
    if value == nil then value = args[name:gsub("-", "_")] end
    return value
end

-- `--flash-on` opts into a live Firefox Flash. The CLI maps
-- `--flash-on VALUE` onto the `flash-on` driver argument (coerced to a Lua
-- boolean); direct `main(args)` callers may use `["flash-on"]` or `flash_on`
-- with a boolean or a truthy string ("true"/"1"/"yes"/"on"). Either spelling
-- being truthy enables the Flash, so a `--flash-on true` CLI run is not
-- cancelled by the companion `flash_on` default of false.
local function flash_on_requested(args)
    args = args or {}
    local candidates = { args["flash-on"], args.flash_on }
    for _, raw in ipairs(candidates) do
        if raw == true then return true end
        if type(raw) == "number" and raw ~= 0 then return true end
        if type(raw) == "string" then
            local lower = raw:lower()
            if lower == "true" or lower == "1" or lower == "yes" or lower == "on" then
                return true
            end
        end
    end
    return false
end

-- Extracts network entries from a `flash_data` JSON document. Returns an
-- empty list unless the document is a `network_info` data response; never
-- throws and never logs payload content.
local function parse_beacon_network_entries(text)
    local entries = {}
    if type(text) ~= "string" or text == "" then return entries end
    if not text:find('"data_type"%s*:%s*"network_info"') then return entries end
    local payload = text:match('"payload"%s*:%s*(%b{})')
    if not payload then return entries end
    local list = payload:match('"entries"%s*:%s*(%b[])')
    if not list then return entries end
    for chunk in list:gmatch("%b{}") do
        local method = chunk:match('"method"%s*:%s*"([^"]+)"')
        local entry_url = chunk:match('"url"%s*:%s*"([^"]+)"')
        local entry_type = chunk:match('"type"%s*:%s*"([^"]+)"')
        if type(entry_url) == "string" and entry_url ~= "" then
            entries[#entries + 1] = {
                method = method or "GET",
                url = entry_url,
                type = entry_type or "",
            }
        end
    end
    return entries
end

local function beacon_endpoint_for(entry, source_url)
    local raw_url = tostring(entry.url or "")
    if raw_url == "" then return nil end
    if not raw_url:match("^https?://") then return nil end
    if no_xcors_only and not is_booking_tld_url(raw_url) then return nil end
    local method = tostring(entry.method or "get"):lower()
    if method == "" then method = "get" end
    return {
        url = raw_url,
        path = path_from_url(raw_url),
        method = method,
        source = source_url,
        discovery_method = "beacon-network",
        confidence = 0.85,
        parameters = {},
        notes = {"heuristically observed in Firefox via Beacon network Flash; heuristic, not authoritative."}
    }
end

local function find_beaconctl(args)
    local override = beacon_arg(args, "beacon_binary")
    if type(override) == "string" and override ~= "" then return override end
    local candidate = repo_root .. "/build/default/plugins/beacon/beaconctl"
    local probe = io.open(candidate, "rb")
    if probe then probe:close(); return candidate end
    return "beaconctl"
end

-- Runs a beaconctl command and returns trimmed stdout, or nil on any
-- failure. Never raises; never writes the command or its output anywhere.
local function run_beacon_capture(command)
    local ok, handle = pcall(function() return io.popen(command, "r") end)
    if not ok or handle == nil then return nil end
    local ok_read, output = pcall(function() return handle:read("*a") end)
    pcall(function() handle:close() end)
    if not ok_read or type(output) ~= "string" then return nil end
    return trim(output)
end

local function extract_flash_id(text)
    if type(text) ~= "string" then return nil end
    return text:match('"flash_id"%s*:%s*"([^"]+)"')
end

-- Live Beacon collection through the local broker. Registers a network_info
-- Flash, prompts for the user-consented addon flow, then polls the bounded
-- delivery queue. Bounded and best-effort: any failure yields zero entries
-- with a short note. Only used for live runs when `--flash-on true` is
-- passed (or, for backward compatibility, when `beacon_socket` is set
-- explicitly); offline `html` runs never flash so they stay deterministic.
local function live_beacon_network_entries(args, page_url)
    local flash_on = flash_on_requested(args)
    local socket = beacon_arg(args, "beacon_socket")
    local socket_explicit = type(socket) == "string" and socket ~= ""
    if not flash_on and not socket_explicit then return {}, "disabled" end
    if not socket_explicit then socket = "/tmp/beacond.sock" end
    if beacon_spec ~= nil then
        local valid, _ = beacon_spec.validate(
            { url_pattern = page_url, resource_types = { "main_frame" } },
            "network_info")
        if not valid then return {}, "invalid filter" end
    end
    local binary = find_beaconctl(args)
    local pattern = beacon_arg(args, "beacon_url_pattern")
    if type(pattern) ~= "string" or pattern == "" then
        local origin = url_origin(page_url)
        pattern = (origin or page_url) .. "/*"
    end
    local flash_out = run_beacon_capture(
        shell_quote(binary) .. " --socket " .. shell_quote(socket) ..
        " flash network_info " .. shell_quote(pattern))
    local flash_id = extract_flash_id(flash_out)
    if not flash_id or flash_id == "" then
        return {}, "flash registration failed or beacond unavailable"
    end
    io.stderr:write("booking-dotcom-admin: Beacon Flash " .. flash_id ..
        " seeking; List Flashes, Connect to Flash on the matching tab, " ..
        "then Send Network Info in Firefox.\n")
    local max_polls = tonumber(beacon_arg(args, "beacon_max_polls")) or 10
    if max_polls < 1 then max_polls = 1 end
    if max_polls > 30 then max_polls = 30 end
    local sleep_secs = tonumber(beacon_arg(args, "beacon_poll_sleep")) or 2
    if sleep_secs < 0 then sleep_secs = 0 end
    if sleep_secs > 10 then sleep_secs = 10 end
    local payload = nil
    for _ = 1, max_polls do
        local poll_out = run_beacon_capture(
            shell_quote(binary) .. " --socket " .. shell_quote(socket) ..
            " poll " .. shell_quote(flash_id))
        if type(poll_out) == "string" and poll_out ~= "" then
            if poll_out:find('"type"%s*:%s*"flash_data"') then
                payload = poll_out
                break
            elseif not poll_out:find('"type"%s*:%s*"flash_empty"') then
                break
            end
        else
            break
        end
        if sleep_secs > 0 then
            pcall(function()
                os.execute("sleep " .. tostring(math.floor(sleep_secs)))
            end)
        end
    end
    pcall(function()
        os.execute(shell_quote(binary) .. " --socket " .. shell_quote(socket) ..
            " disconnect " .. shell_quote(flash_id) .. " >/dev/null 2>&1")
    end)
    if type(payload) ~= "string" or payload == "" then
        return {}, "no Beacon network data delivered"
    end
    return parse_beacon_network_entries(payload), "live"
end

-- Merges Beacon network results into the endpoint list. Hermetic inputs
-- (`beacon_json`) work offline and in tests; the live Flash path is used
-- only for live runs with `--flash-on true` (or an explicit `beacon_socket`
-- for backward compatibility) and never for offline `html` runs. Never throws.
local function apply_beacon_network(endpoints, args, page_url,
                                    authenticated_flag, offline)
    local status = { used = false, entries = 0, note = "disabled" }
    local ok, result = pcall(function()
        local injected = beacon_arg(args, "beacon_json")
        local entries = {}
        local origin = "disabled"
        if type(injected) == "string" and injected ~= "" then
            entries = parse_beacon_network_entries(injected)
            origin = "injected"
        elseif offline then
            return status
        else
            entries, origin = live_beacon_network_entries(args, page_url)
        end
        if #entries == 0 then
            status.note = (origin == "injected") and "injected payload held no network entries" or
                (type(origin) == "string" and origin or "no Beacon network data delivered")
            return status
        end
        local seen = {}
        for _, ep in ipairs(endpoints or {}) do
            if type(ep) == "table" and ep.url then
                seen[endpoint_key(ep)] = true
            end
        end
        local added = 0
        for _, entry in ipairs(entries) do
            local ep = beacon_endpoint_for(entry, page_url)
            if ep ~= nil then
                local key = endpoint_key(ep)
                if not seen[key] then
                    seen[key] = true
                    endpoints[#endpoints + 1] = ep
                    added = added + 1
                end
            end
        end
        for _, ep in ipairs(endpoints or {}) do
            if type(ep) == "table" then ep.authenticated = authenticated_flag == true end
        end
        status.used = added > 0
        status.entries = added
        status.note = origin .. ": " .. tostring(added) ..
            " network endpoint(s) merged"
        return status
    end)
    if not ok then
        status.used = false
        status.note = "beacon merge failed; seeds kept"
        return status
    end
    return result
end

-- OpenCode endpoint cleanup (optional, opt-in).
--
-- After filtering, the redacted endpoint list can be sent to a local OpenCode
-- agent for a subtractive cleanup pass: drop static leftovers, beacon/
-- telemetry pings, and duplicates the heuristics kept. The agent may only
-- REMOVE entries. The driver intersects the answer with the scraped
-- (method, url) set and keeps original endpoint tables verbatim, so invented
-- URLs can never enter the specs. Two ways to enable:
--   * `opencode_json` — a pre-baked agent answer (JSON array). Hermetic:
--     no network, no native module, works offline and in tests.
--   * `opencode`/`opencode_enabled` true — live call through the `lopencode`
--     native module against OPENCODE_BASE_URL (default
--     http://127.0.0.1:4096). Credentials come from OPENCODE_SERVER_USERNAME /
--     OPENCODE_SERVER_PASSWORD process environment only, never driver args.
-- Disabled by default; offline `html` runs never touch the network unless
-- `opencode_json` is given. Never throws: any failure keeps the seeds.
local function opencode_enabled_requested(args)
    args = args or {}
    local candidates = {
        args.opencode, args.opencode_enabled,
        args["opencode-enabled"], args["opencode_enabled"]
    }
    for _, raw in ipairs(candidates) do
        if raw == true then return true end
        if type(raw) == "number" and raw ~= 0 then return true end
        if type(raw) == "string" then
            local lower = raw:lower()
            if lower == "true" or lower == "1" or lower == "yes" or lower == "on" then
                return true
            end
        end
    end
    return false
end

local function opencode_json_quote(value)
    return '"' .. tostring(value):gsub('[%z\1-\31\\"]', function(ch)
        if ch == '"' then return '\\"' end
        if ch == '\\' then return '\\\\' end
        return string.format('\\u%04x', ch:byte())
    end) .. '"'
end

-- Serializes endpoints to a compact JSON array for the agent. Only redacted
-- discovery fields are included. Caps count and bytes; excess is reported via
-- the truncated flag so the status note stays honest.
local function opencode_encode_endpoints(endpoints, max_count, max_bytes)
    max_count = tonumber(max_count) or 200
    if max_count < 1 then max_count = 1 end
    if max_count > 2000 then max_count = 2000 end
    max_bytes = tonumber(max_bytes) or 131072
    if max_bytes < 1024 then max_bytes = 1024 end
    if max_bytes > 1048576 then max_bytes = 1048576 end
    local parts, count, truncated, size = {}, 0, false, 2
    for _, ep in ipairs(endpoints or {}) do
        if type(ep) == "table" and type(ep.url) == "string" and ep.url ~= "" then
            local item = '{"url":' .. opencode_json_quote(ep.url) ..
                ',"method":' .. opencode_json_quote(tostring(ep.method or "get"):lower()) ..
                ',"path":' .. opencode_json_quote(ep.path or "") ..
                ',"discovery_method":' .. opencode_json_quote(ep.discovery_method or "") ..
                ',"confidence":' .. tostring(tonumber(ep.confidence) or 0) .. '}'
            if count >= max_count or size + #item + 1 > max_bytes then
                truncated = true
                break
            end
            parts[#parts + 1] = item
            size = size + #item + 1
            count = count + 1
        end
    end
    return "[" .. table.concat(parts, ",") .. "]", count, truncated
end

-- Minimal bounded JSON decoder for agent answers: objects, arrays, strings
-- (with escapes), numbers, true/false/null. Depth-capped, length-capped.
-- Returns value + kind ("object", "array", "string", "number", "boolean",
-- "null") or nil plus a short reason. Never throws.
local function opencode_json_decode(text)
    if type(text) ~= "string" or text == "" then return nil, "empty" end
    if #text > 524288 then return nil, "too large" end
    local pos, depth = 1, 0
    local parse_value
    local function skip_ws()
        while pos <= #text and text:sub(pos, pos):match("%s") do pos = pos + 1 end
    end
    local function parse_string()
        -- pos is on the opening quote.
        pos = pos + 1
        local out, escaped = {}, false
        while pos <= #text do
            local ch = text:sub(pos, pos)
            if escaped then
                if ch == "u" then
                    local hex = text:sub(pos + 1, pos + 4)
                    local code = tonumber(hex, 16)
                    if not hex:match("^%x%x%x%x$") or not code then return nil end
                    if code < 128 then out[#out + 1] = string.char(code) end
                    pos = pos + 4
                elseif ch == '"' then out[#out + 1] = '"'
                elseif ch == '\\' then out[#out + 1] = '\\'
                elseif ch == '/' then out[#out + 1] = '/'
                elseif ch == 'b' then out[#out + 1] = '\b'
                elseif ch == 'f' then out[#out + 1] = '\f'
                elseif ch == 'n' then out[#out + 1] = '\n'
                elseif ch == 'r' then out[#out + 1] = '\r'
                elseif ch == 't' then out[#out + 1] = '\t'
                else return nil end
                escaped = false
            elseif ch == "\\" then
                escaped = true
            elseif ch == '"' then
                pos = pos + 1
                return table.concat(out), "string"
            else
                out[#out + 1] = ch
            end
            pos = pos + 1
        end
        return nil
    end
    local function parse_object()
        depth = depth + 1
        if depth > 6 then return nil end
        pos = pos + 1
        local obj = {}
        skip_ws()
        if text:sub(pos, pos) == "}" then pos = pos + 1; depth = depth - 1; return obj, "object" end
        while true do
            skip_ws()
            if text:sub(pos, pos) ~= '"' then return nil end
            local key = parse_string()
            if type(key) ~= "string" then return nil end
            skip_ws()
            if text:sub(pos, pos) ~= ":" then return nil end
            pos = pos + 1
            local value = parse_value()
            if value == nil then
                -- Only JSON null decodes to nil here; verify the literal so
                -- truncated input fails closed instead of storing nil.
                skip_ws()
                local sep = text:sub(pos, pos)
                if sep ~= "," and sep ~= "}" then return nil end
            end
            obj[key] = value
            skip_ws()
            local sep = text:sub(pos, pos)
            if sep == "," then pos = pos + 1
            elseif sep == "}" then pos = pos + 1; depth = depth - 1; return obj, "object"
            else return nil end
        end
    end
    local function parse_array()
        depth = depth + 1
        if depth > 6 then return nil end
        pos = pos + 1
        local arr = {}
        skip_ws()
        if text:sub(pos, pos) == "]" then pos = pos + 1; depth = depth - 1; return arr, "array" end
        while true do
            local value = parse_value()
            if value == nil then
                skip_ws()
                local sep = text:sub(pos, pos)
                if sep ~= "," and sep ~= "]" then return nil end
            end
            arr[#arr + 1] = value
            skip_ws()
            local sep = text:sub(pos, pos)
            if sep == "," then pos = pos + 1
            elseif sep == "]" then pos = pos + 1; depth = depth - 1; return arr, "array"
            else return nil end
        end
    end
    local function parse_literal()
        for _, word in ipairs({"true", "false", "null"}) do
            if text:sub(pos, pos + #word - 1) == word then
                pos = pos + #word
                if word == "null" then return nil, "null" end
                return word == "true", "boolean"
            end
        end
        local num = text:match("^-?%d+%.?%d*[eE]?[+-]?%d*", pos)
        if num ~= nil and num ~= "" and num ~= "-" then
            pos = pos + #num
            return tonumber(num) or 0, "number"
        end
        return nil
    end
    parse_value = function()
        skip_ws()
        local ch = text:sub(pos, pos)
        if ch == "{" then return parse_object()
        elseif ch == "[" then return parse_array()
        elseif ch == '"' then return parse_string()
        elseif ch == "" then return nil
        else return parse_literal() end
    end
    local value, kind = parse_value()
    if value == nil and kind ~= "null" then return nil, "invalid" end
    skip_ws()
    if pos <= #text then return nil, "trailing data" end
    return value, kind or "null"
end

-- Validates an agent cleanup answer and intersects it with the scraped set.
-- Accepts a bare JSON array or an object wrapping it under "endpoints"/"keep".
-- Returns the cleaned list (original tables, verbatim) plus kept/dropped/
-- invented counts, or nil plus a reason. Empty keep-lists are rejected: an
-- agent that keeps nothing is treated as a failed cleanup, never as proof
-- that every endpoint is junk. Never throws.
local function opencode_apply_answer(endpoints, answer_text)
    local known = {}
    for _, ep in ipairs(endpoints or {}) do
        if type(ep) == "table" and type(ep.url) == "string" and ep.url ~= "" then
            known[tostring(ep.method or "get"):lower() .. "\0" .. ep.url] = ep
        end
    end
    local decoded, kind = opencode_json_decode(answer_text)
    if decoded == nil then return nil, "agent answer is not valid JSON" end
    local items = nil
    if kind == "array" then
        items = decoded
    elseif kind == "object" then
        for _, key in ipairs({"endpoints", "keep"}) do
            if type(decoded[key]) == "table" then
                items = decoded[key]
                break
            end
        end
    end
    if type(items) ~= "table" then return nil, "agent answer is not a JSON array" end
    local is_array = true
    do
        local count = 0
        for k in pairs(items) do
            if type(k) ~= "number" then is_array = false; break end
            count = count + 1
        end
        if not is_array or count ~= #items then
            return nil, "agent answer is not a JSON array"
        end
    end
    if #items == 0 then return nil, "agent answer keeps no endpoints" end
    if #items > 5000 then return nil, "agent answer exceeds item budget" end
    local cleaned, seen, invented = {}, {}, 0
    for _, item in ipairs(items) do
        if type(item) ~= "table" or type(item.url) ~= "string" or item.url == "" then
            return nil, "agent answer holds a non-endpoint item"
        end
        local key = tostring(item.method or "get"):lower() .. "\0" .. item.url
        local original = known[key]
        if original == nil then
            invented = invented + 1
        elseif not seen[key] then
            seen[key] = true
            cleaned[#cleaned + 1] = original
        end
    end
    local original_count = 0
    for _ in pairs(known) do original_count = original_count + 1 end
    return cleaned, { kept = #cleaned, dropped = original_count - #cleaned, invented = invented }
end

-- Fallback cleanup prompt when the loaded lopencode module predates
-- build_cleanup_prompt. Same subtractive contract as the native builder.
local function opencode_fallback_prompt(endpoints_json, instructions)
    local prompt =
        "You are cleaning a heuristically scraped web-API endpoint list. " ..
        "Return a JSON array containing ONLY a subset of the input objects, " ..
        "copied verbatim (same url and method values). NEVER invent, " ..
        "normalize, or rewrite urls or methods. DROP static assets, " ..
        "analytics/beacon pings, challenge/telemetry URLs, plain pages, and " ..
        "duplicates. When in doubt, KEEP the entry. " ..
        "Return the JSON array only, no prose.\n"
    if type(instructions) == "string" and instructions ~= "" then
        prompt = prompt .. "Additional instructions: " .. instructions .. "\n"
    end
    return prompt .. "endpoints:\n" .. endpoints_json
end

-- Loads the lopencode native module: explicit `opencode_module` loadlib path
-- first (deterministic), then a plain require (works when the host preloads
-- the native module), then the in-repo dev build beside the driver. Returns
-- nil when unavailable; never throws.
local function opencode_load_module(args)
    local path = args.opencode_module or args["opencode-module"]
    if type(path) == "string" and path ~= "" then
        local ok_load, loader = pcall(function()
            return assert(package.loadlib(path, "luaopen_lopencode"))
        end)
        if ok_load and type(loader) == "function" then
            local ok_call, module = pcall(loader)
            if ok_call and type(module) == "table" then return module end
        end
        return nil
    end
    local ok, module = pcall(require, "lopencode")
    if ok and type(module) == "table" and type(module.client) == "table" and
       type(module.client.new) == "function" then
        return module
    end
    local candidate = repo_root .. "/build/default/plugins/opencode-bridge/lopencode.so"
    local probe = io.open(candidate, "rb")
    if probe ~= nil then
        probe:close()
        local ok_load, loader = pcall(function()
            return assert(package.loadlib(candidate, "luaopen_lopencode"))
        end)
        if ok_load and type(loader) == "function" then
            local ok_call, fallback = pcall(loader)
            if ok_call and type(fallback) == "table" and
               type(fallback.client) == "table" and
               type(fallback.client.new) == "function" then
                return fallback
            end
        end
    end
    return nil
end

-- Live cleanup through an OpenCode server. Returns the agent answer string or
-- nil plus a reason. Credentials come from process environment only; the prompt
-- carries redacted endpoints alone. Never throws, never logs secrets.
local function opencode_live_answer(module, endpoints_json, args)
    local base_url = args.opencode_base_url or args["opencode-base-url"]
    if type(base_url) ~= "string" or base_url == "" then
        base_url = os.getenv("OPENCODE_BASE_URL") or "http://127.0.0.1:4096"
    end
    local instructions = args.opencode_instructions or args["opencode-instructions"] or ""
    local prompt = nil
    if type(module.build_cleanup_prompt) == "function" then
        local ok, text = pcall(function()
            return module.build_cleanup_prompt(endpoints_json, tostring(instructions or ""))
        end)
        if ok and type(text) == "string" and text ~= "" then prompt = text end
    end
    if prompt == nil then
        prompt = opencode_fallback_prompt(endpoints_json, instructions)
    end
    local new_client = (module.client and module.client.new) or module.new
    if type(new_client) ~= "function" then return nil, "lopencode module has no client constructor" end
    local api_prefix = args.opencode_api_prefix or args["opencode-api-prefix"]
    if type(api_prefix) ~= "string" or api_prefix == "" then api_prefix = "/api" end
    local ok, answer = pcall(function()
        local client = assert(new_client({
            base_url = base_url,
            username = os.getenv("OPENCODE_SERVER_USERNAME"),
            password = os.getenv("OPENCODE_SERVER_PASSWORD"),
            timeout_ms = tonumber(args.opencode_timeout_ms) or
                tonumber(args["opencode-timeout-ms"]) or 30000,
            max_requests = tonumber(args.opencode_max_requests) or
                tonumber(args["opencode-max-requests"]) or 32,
            prompt_wait_ms = tonumber(args.opencode_wait_ms) or
                tonumber(args["opencode-wait-ms"]) or 120000,
            api_prefix = api_prefix,
            allow_remote_http = args.opencode_allow_remote_http == true or
                tostring(args.opencode_allow_remote_http or ""):lower() == "true",
        }))
        local session_id = assert(client:create_session())
        local result = assert(client:prompt(session_id, prompt))
        pcall(function() client:close() end)
        return result
    end)
    if not ok then return nil, "live cleanup request failed" end
    if type(answer) ~= "string" or answer == "" then
        return nil, "live cleanup returned no answer"
    end
    return answer
end

-- Applies the OpenCode cleanup stage. Returns the endpoint list to export
-- (original tables) plus a status table for the x-prowsetk-opencode block.
-- Hermetic `opencode_json` works offline and in tests; live calls need
-- `opencode`/`opencode_enabled` and the native module. Never throws.
local function apply_opencode_cleanup(endpoints, args, offline)
    local status = { used = false, kept = 0, dropped = 0, invented = 0, note = "disabled" }
    local ok, cleaned, updated = pcall(function()
        if type(endpoints) ~= "table" or #endpoints == 0 then
            status.note = "no endpoints to clean"
            return endpoints or {}, status
        end
        local injected = args.opencode_json or args["opencode-json"]
        local flag_enabled = opencode_enabled_requested(args)
        local enabled = flag_enabled or
            (type(injected) == "string" and injected ~= "")
        if not enabled then return endpoints, status end
        local answer, origin = nil, "live"
        if type(injected) == "string" and injected ~= "" then
            answer, origin = injected, "injected"
            status.note = ""
        elseif offline and not flag_enabled then
            status.note = "live cleanup disabled for offline runs; pass opencode_json for hermetic cleanup"
            return endpoints, status
        else
            local module = opencode_load_module(args or {})
            if module == nil then
                status.note = "lopencode module unavailable; seeds kept"
                return endpoints, status
            end
            local encoded, count, truncated = opencode_encode_endpoints(
                endpoints, args.opencode_max_endpoints or args["opencode-max-endpoints"])
            if count == 0 then
                status.note = "no endpoints to clean"
                return endpoints, status
            end
            if truncated then
                status.note = "input truncated to " .. tostring(count) .. " endpoints; "
            else
                status.note = ""
            end
            local failure = nil
            answer, failure = opencode_live_answer(module, encoded, args or {})
            if answer == nil then
                status.note = tostring(failure or "live cleanup failed") .. "; seeds kept"
                return endpoints, status
            end
        end
        local cleaned, stats = opencode_apply_answer(endpoints, answer)
        if cleaned == nil then
            status.note = tostring(stats) .. "; seeds kept"
            return endpoints, status
        end
        status.used = true
        status.kept = stats.kept
        status.dropped = stats.dropped
        status.invented = stats.invented
        local audit = args.opencode_output or args["opencode-output"]
        if type(audit) == "string" and audit ~= "" then
            pcall(function()
                local file = assert(io.open(audit, "wb"))
                file:write(answer or "")
                file:close()
            end)
        end
        status.note = (status.note or "") .. origin .. ": kept " ..
            tostring(stats.kept) .. ", dropped " .. tostring(stats.dropped) ..
            ", invented-ignored " .. tostring(stats.invented)
        return cleaned, status
    end)
    if not ok then
        status.used = false
        status.note = "opencode cleanup failed; seeds kept"
        return endpoints, status
    end
    return cleaned, updated
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

-- Collects quoted `.js` references (webpack chunks, dynamic imports by
-- path) resolved against a base URL, for bundle-following. Origin gating
-- is left to the caller via script_fetch_allowed.
local function scan_js_refs(text, base_url)
    local found, seen = {}, {}
    local function add(candidate)
        local bare = candidate:gsub("[?#].*$", "")
        if not bare:lower():match("%.js$") then return end
        local resolved = resolve_url(base_url, candidate)
        if resolved and not seen[resolved] then
            seen[resolved] = true
            found[#found + 1] = resolved
        end
    end
    text = text or ""
    for quoted in text:gmatch('"([^"]+)"') do add(quoted) end
    for quoted in text:gmatch("'([^']+)'") do add(quoted) end
    return found
end

-- Method-aware POST scan over a fetched body (external script bundle or API
-- response page). Detects POST forms, fetch/XHR POSTs, sendBeacon calls,
-- shorthand POST helpers ($.post / axios.post / minified equivalents) and
-- $.ajax with type/method POST. Returns POST endpoint tables only.
--
-- Unlike scan_urls (which is same-origin GET-only), cross-origin POST targets
-- are kept: an explicit POST to any URL is backend evidence, and the final
-- --no-xcors filter still restricts output to the booking.com TLD when asked.
-- API-pattern gating is intentionally absent: an explicit non-GET method
-- outweighs the path heuristic, mirroring the plugin filter rule.
local function scan_post_endpoints(text, base_url, source_url)
    source_url = source_url or base_url
    local found, seen = {}, {}
    local function add_post(candidate, discovery_method, note, content_type, confidence)
        local resolved = resolve_url(base_url, candidate)
        if not resolved or resolved == "" then return end
        -- Reject scheme-only fragments from string concatenation
        -- (e.g. "https://" .. host): absolute URLs need a real host.
        if resolved:match("^https?://") and (url_host(resolved) or "") == "" then return end
        if no_xcors_only and not is_booking_tld_url(resolved) then return end
        local key = "post\0" .. resolved
        if seen[key] then return end
        seen[key] = true
        found[#found + 1] = {
            url = resolved,
            path = path_from_url(resolved),
            method = "post",
            source = source_url,
            discovery_method = discovery_method,
            confidence = confidence,
            parameters = {},
            request_content_type = content_type,
            notes = {note}
        }
    end
    text = text or ""

    -- POST forms (fetched HTML bodies).
    for tag in text:gmatch("<[Ff][Oo][Rr][Mm][^>]*>") do
        local action = tag:match('[Aa][Cc][Tt][Ii][Oo][Nn]%s*=%s*"([^"]+)"')
            or tag:match("[Aa][Cc][Tt][Ii][Oo][Nn]%s*=%s*'([^']+)'")
            or tag:match("[Aa][Cc][Tt][Ii][Oo][Nn]%s*=%s*([^%s>]+)")
        local method = tag:match('[Mm][Ee][Tt][Hh][Oo][Dd]%s*=%s*"([^"]+)"')
            or tag:match("[Mm][Ee][Tt][Hh][Oo][Dd]%s*=%s*'([^']+)'")
            or tag:match("[Mm][Ee][Tt][Hh][Oo][Dd]%s*=%s*([^%s>]+)")
        if trim(method or ""):lower() == "post" then
            add_post(action or base_url, "html-form",
                "heuristically discovered POST form in fetched body by Booking.com admin driver",
                "application/x-www-form-urlencoded", 0.75)
        end
    end

    -- fetch(url, {... method: "POST" ...}): double-, single- and backtick-
    -- quoted URLs, quoted or bare option keys. The options window is bounded
    -- so an unrelated POST mention further down cannot flip a GET fetch.
    -- Also covers fetch(new Request("url", {method: "POST"})).
    do
        local pos = 1
        while true do
            local _, e = text:find("[Ff][Ee][Tt][Cc][Hh]%s*%(", pos)
            if not e then break end
            local window = text:sub(e + 1, e + 1500)
            local url = window:match('^%s*"([^"]+)"')
                or window:match("^%s*'([^']+)'")
                or window:match("^%s*`([^`]+)`")
                or window:match('^[Nn][Ee][Ww]%s+[Rr][Ee][Qq][Uu][Ee][Ss][Tt]%s*%(%s*"([^"]+)"')
                or window:match("^[Nn][Ee][Ww]%s+[Rr][Ee][Qq][Uu][Ee][Ss][Tt]%s*%(%s*'([^']+)'")
            if url then
                local check = window:sub(1, #url + 320):lower()
                if check:find('["\']?method["\']?%s*:%s*["\']post["\']') then
                    add_post(url, "script-post",
                        "heuristically discovered fetch POST in script by Booking.com admin driver",
                        nil, 0.65)
                end
            end
            pos = e + 1
        end
    end

    -- XHR open(method, url): double- and single-quoted forms.
    for method, url in text:gmatch('%.[Oo][Pp][Ee][Nn]%s*%(%s*"([^"]+)"%s*,%s*"([^"]+)"') do
        if trim(method):lower() == "post" then
            add_post(url, "script-post",
                "heuristically discovered XHR POST in script by Booking.com admin driver",
                nil, 0.65)
        end
    end
    for method, url in text:gmatch("%.[Oo][Pp][Ee][Nn]%s*%(%s*'([^']+)'%s*,%s*'([^']+)'") do
        if trim(method):lower() == "post" then
            add_post(url, "script-post",
                "heuristically discovered XHR POST in script by Booking.com admin driver",
                nil, 0.65)
        end
    end

    -- navigator.sendBeacon(url, ...): always POST; the usual carrier for
    -- challenge/telemetry beacons.
    for url in text:gmatch('[Ss][Ee][Nn][Dd][Bb][Ee][Aa][Cc][Oo][Nn]%s*%(%s*"([^"]+)"') do
        add_post(url, "script-post",
            "heuristically discovered sendBeacon POST in script by Booking.com admin driver",
            nil, 0.65)
    end
    for url in text:gmatch("[Ss][Ee][Nn][Dd][Bb][Ee][Aa][Cc][Oo][Nn]%s*%(%s*'([^']+)'") do
        add_post(url, "script-post",
            "heuristically discovered sendBeacon POST in script by Booking.com admin driver",
            nil, 0.65)
    end
    for url in text:gmatch("[Ss][Ee][Nn][Dd][Bb][Ee][Aa][Cc][Oo][Nn]%s*%(%s*`([^`]+)`") do
        add_post(url, "script-post",
            "heuristically discovered sendBeacon POST in script by Booking.com admin driver",
            nil, 0.65)
    end

    -- Shorthand POST helpers: $.post(url), axios.post(url) and minified
    -- equivalents (any receiver). Double-, single- and backtick-quoted.
    for url in text:gmatch('%.[Pp][Oo][Ss][Tt]%s*%(%s*"([^"]+)"') do
        add_post(url, "script-post",
            "heuristically discovered shorthand POST in script by Booking.com admin driver",
            nil, 0.65)
    end
    for url in text:gmatch("%.[Pp][Oo][Ss][Tt]%s*%(%s*'([^']+)'") do
        add_post(url, "script-post",
            "heuristically discovered shorthand POST in script by Booking.com admin driver",
            nil, 0.65)
    end
    for url in text:gmatch("%.[Pp][Oo][Ss][Tt]%s*%(%s*`([^`]+)`") do
        add_post(url, "script-post",
            "heuristically discovered shorthand POST in script by Booking.com admin driver",
            nil, 0.65)
    end

    -- $.ajax({url: "...", type: "POST"}) (or method: "POST"), either key
    -- order, either quote style.
    do
        local pos = 1
        while true do
            local _, e = text:find("%.[Aa][Jj][Aa][Xx]%s*%(", pos)
            if not e then break end
            local window = text:sub(e + 1, e + 1200)
            local lowered = window:lower()
            if lowered:find('["\']?type["\']?%s*:%s*["\']post["\']', 1)
                or lowered:find('["\']?method["\']?%s*:%s*["\']post["\']', 1) then
                local url = window:match('[Uu][Rr][Ll]%s*:%s*"([^"]+)"')
                    or window:match("[Uu][Rr][Ll]%s*:%s*'([^']+)'")
                if url then
                    add_post(url, "script-post",
                        "heuristically discovered ajax POST in script by Booking.com admin driver",
                        nil, 0.65)
                end
            end
            pos = e + 1
        end
    end

    -- Generic options-object POST scan: anchors on each method:/type: "POST"
    -- literal and pairs it with the nearest url:/uri: literal within a
    -- bounded window. Catches config-object clients of any name
    -- (axios.request, fresa, ...) regardless of key order; pairs farther
    -- apart are left alone to avoid matching unrelated literals in dense
    -- bundles. Dotted constant references (url: NS.VALIDATE_URL) resolve
    -- through the per-body constant map built below.
    local consts = {}
    for name, val in text:gmatch('([A-Za-z_$][%w$_]*)%s*:%s*"(/[^"]+)"') do
        if consts[name] == nil and not val:match("%.js$") then consts[name] = val end
    end
    for name, val in text:gmatch("([A-Za-z_$][%w$_]*)%s*:%s*'(/[^']+)'") do
        if consts[name] == nil and not val:match("%.js$") then consts[name] = val end
    end
    for name, val in text:gmatch('([A-Za-z_$][%w$_]*)%s*:%s*"(https?://[^"]+)"') do
        if consts[name] == nil then consts[name] = val end
    end
    local lowered_all = text:lower()
    local function key_start_ok(at)
        if at <= 1 then return true end
        local prev = lowered_all:sub(at - 1, at - 1)
        return prev == '"' or prev == "'" or prev == "{" or prev == "," or
               prev == " " or prev == "\t" or prev == "\n" or prev == "("
    end
    local function read_literal(at)
        local q = at
        while q <= #text and text:sub(q, q):match("%s") do q = q + 1 end
        local quote = text:sub(q, q)
        if quote ~= '"' and quote ~= "'" and quote ~= "`" then return nil end
        local close = text:find(quote, q + 1, true)
        if close == nil then return nil end
        return text:sub(q + 1, close - 1)
    end
    local function url_literal_ok(url)
        if type(url) ~= "string" or url == "" then return false end
        if url:sub(1, 1) == "/" then
            return not url:match("%.js$") and not url:match("%.css$")
        end
        return url:lower():match("^https?://") ~= nil
    end
    do
        local pos = 1
        while true do
            local ms = lowered_all:find("method", pos, true)
            local ts = lowered_all:find("type", pos, true)
            local ks
            if ms == nil then ks = ts
            elseif ts == nil then ks = ms
            else ks = (ms < ts) and ms or ts end
            if ks == nil then break end
            pos = ks + 6
            if key_start_ok(ks) then
                local colon = lowered_all:find(":", ks + 6, true)
                if colon ~= nil and colon - ks <= 14 then
                    local value = read_literal(colon + 1)
                    if value ~= nil and value:lower() == "post" then
                        local from = math.max(1, ks - 800)
                        local upto = math.min(#text, ks + 400)
                        local best, best_dist, best_const = nil, nil, false
                        local function consider(name_at)
                            if not key_start_ok(name_at) then return end
                            local ucolon = lowered_all:find(":", name_at + 3, true)
                            if ucolon == nil or ucolon - name_at > 14 or ucolon > upto then return end
                            local uval = read_literal(ucolon + 1)
                            local resolved_val, via_const = nil, false
                            if url_literal_ok(uval) then
                                resolved_val = uval
                            elseif uval == nil then
                                local r = ucolon + 1
                                while r <= #text and text:sub(r, r):match("%s") do r = r + 1 end
                                local ref = text:sub(r, r + 200):match("^([A-Za-z_$][%w$._]*[%w$_])")
                                if ref ~= nil then
                                    local leaf = ref:match("[%w$_]+$")
                                    if leaf ~= nil and consts[leaf] ~= nil then
                                        resolved_val, via_const = consts[leaf], true
                                    end
                                end
                            end
                            if resolved_val ~= nil then
                                local dist = (name_at > ks) and (name_at - ks) or (ks - name_at)
                                if best_dist == nil or dist < best_dist then
                                    best, best_dist, best_const = resolved_val, dist, via_const
                                end
                            end
                        end
                        local upos = from
                        while true do
                            local um = lowered_all:find("url", upos, true)
                            local im = lowered_all:find("uri", upos, true)
                            local nxt
                            if um == nil then nxt = im
                            elseif im == nil then nxt = um
                            else nxt = (um < im) and um or im end
                            if nxt == nil or nxt >= upto then break end
                            consider(nxt)
                            upos = nxt + 1
                        end
                        if best ~= nil then
                            add_post(best, "script-post",
                                "heuristically discovered options-object POST in script by Booking.com admin driver" ..
                                (best_const and " (via constant reference)" or ""),
                                nil, best_const and 0.55 or 0.65)
                        end
                    end
                end
            end
        end
    end

    return found
end

local function inspect_scripts(session, document, page_url, endpoints, seen_endpoints, authenticated_flag, args)
    if not document or type(document.query_selector_all) ~= "function" then return end
    args = args or {}
    local fetched_scripts = 0
    local max_scripts = 32
    local queue, seen_scripts = {}, {}
    for _, script in ipairs(document:query_selector_all("script[src]")) do
        local src = resolve_url(page_url, script:attribute("src"))
        if src and script_fetch_allowed(page_url, src, args) and not seen_scripts[src] then
            if not no_xcors_only or is_booking_tld_url(src) then
                seen_scripts[src] = true
                queue[#queue + 1] = src
            end
        end
    end
    while #queue > 0 and fetched_scripts < max_scripts do
        local script_url = table.remove(queue, 1)
        fetched_scripts = fetched_scripts + 1
        local ok, body = pcall(function() return select(1, fetch_page(session, script_url)) end)
        if ok then
            for import_ref in body:gmatch("import%s*%(%s*['\"]([^'\"]+)['\"]%s*%)") do
                local imported = resolve_url(script_url, import_ref)
                if imported and script_fetch_allowed(page_url, imported, args) and not seen_scripts[imported] then
                    if not no_xcors_only or is_booking_tld_url(imported) then
                        seen_scripts[imported] = true
                        queue[#queue + 1] = imported
                    end
                end
            end
            -- Follow further same-bundle-relative chunks (webpack code
            -- splitting). Resolved against the bundle URL, since chunks live
            -- next to their parent bundle rather than the page.
            for _, chunk in ipairs(scan_js_refs(body, script_url)) do
                if script_fetch_allowed(page_url, chunk, args) and not seen_scripts[chunk] then
                    if not no_xcors_only or is_booking_tld_url(chunk) then
                        if fetched_scripts + #queue < max_scripts then
                            seen_scripts[chunk] = true
                            queue[#queue + 1] = chunk
                        end
                    end
                end
            end
            -- Method-aware POST scan first: bundle POST calls are strong
            -- evidence, so a URL explicitly POSTed here must not also be
            -- recorded as a low-evidence GET twin from the blind scan below.
            -- Relative references resolve against the page (execution
            -- origin); the bundle URL is kept as provenance source.
            local post_urls = {}
            for _, ep in ipairs(scan_post_endpoints(body, page_url, script_url)) do
                add_endpoint(endpoints, seen_endpoints, ep, authenticated_flag)
                post_urls[ep.url] = true
            end
            for _, url in ipairs(scan_urls(body, page_url)) do
                if no_xcors_only and not is_booking_tld_url(url) then
                    -- skip: --no-xcors keeps booking.com TLD endpoints only
                elseif post_urls[url] then
                    -- skip: already recorded as POST from this same body
                elseif is_api_like(url) then
                    add_endpoint(endpoints, seen_endpoints, make_endpoint(url, script_url), authenticated_flag)
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
        if is_api_like(ep.url) then
            if not no_xcors_only or is_booking_tld_url(ep.url) then
                queue[#queue + 1] = {url = ep.url, depth = 0}
            end
        end
    end
    local count = 0
    while #queue > 0 and count < max_requests do
        local item = table.remove(queue, 1)
        if no_xcors_only and not is_booking_tld_url(item.url) then
            visited[item.url] = true
        elseif not visited[item.url] and same_origin(start_url, item.url) then
            visited[item.url] = true
            count = count + 1
            local ok, body = pcall(function() return select(1, fetch_page(session, item.url)) end)
            if ok and item.depth < max_depth then
                local post_urls = {}
                for _, ep in ipairs(scan_post_endpoints(body, item.url)) do
                    add_endpoint(endpoints, seen_endpoints, ep, authenticated_flag)
                    post_urls[ep.url] = true
                end
                for _, url in ipairs(scan_urls(body, item.url)) do
                    if no_xcors_only and not is_booking_tld_url(url) then
                        -- skip: --no-xcors keeps booking.com TLD endpoints only
                    elseif post_urls[url] then
                        -- skip: already recorded as POST from this same body
                    elseif is_api_like(url) and same_origin(start_url, url) then
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
    local result = scrape_endpoints.scrape(session, spec)
    return result.endpoints or {}
end

local function add_assistant_browser_options(spec, args, url)
    spec.url = url or spec.url
    spec.assistant_browser_enabled = args.assistant_browser_enabled == true or
        tostring(args.assistant_browser_enabled or ""):lower() == "true"
    spec.assistant_prompt = true
    spec.assistant_browser = args.assistant_browser or ""
    spec.assistant_browser_method = args.assistant_browser_method or "manual"
    spec.assistant_browser_endpoint = args.assistant_browser_endpoint or ""
    spec.assistant_browser_debug_port = tonumber(args.assistant_browser_debug_port) or 0
    spec.assistant_wait_timeout_ms = tonumber(args.assistant_browser_wait_timeout_ms) or 300000
    spec.success_beacon = args.success_beacon or ""
    spec.success_beacon_type = args.success_beacon_type or "auto"
    return spec
end

local function crawl(session, start_url, args, authenticated_flag)
    no_xcors_only = no_xcors_requested(args)
    local max_depth = tonumber(args.max_depth) or 2
    local max_pages = tonumber(args.max_pages) or 100
    local max_api_depth = tonumber(args.max_api_depth) or 2
    local max_api_requests = tonumber(args.max_api_requests) or 64
    local endpoints, seen_endpoints = {}, {}
    local visited, queue = {}, {{url = start_url, depth = 0}}
    local pages = 0

    while #queue > 0 and pages < max_pages do
        local item = table.remove(queue, 1)
        if not visited[item.url] and (item.url == start_url or should_crawl_page(start_url, item.url)) then
            visited[item.url] = true
            pages = pages + 1
            local body = select(1, fetch_page(session, item.url))
            local document = load_page(session, item.url, body)
            inspect_scripts(session, document, item.url, endpoints, seen_endpoints, authenticated_flag, args)
            local page_spec = add_assistant_browser_options({
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
            }, args, item.url)
            for _, ep in ipairs(discover(session, item.url, page_spec)) do
                add_endpoint(endpoints, seen_endpoints, ep, authenticated_flag)
            end

            local api_spec = add_assistant_browser_options({
                url = item.url,
                follow_links = true,
                inspect_scripts = true,
                scrape_all_paths = false,
                observe_network = true,
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
            }, args, item.url)
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

local function append_metadata(yaml, authenticated_flag, restful, beacon_status, opencode_status)
    restful = restful or {}
    beacon_status = beacon_status or {}
    opencode_status = opencode_status or {}
    return yaml ..
        "x-prowsetk-booking-admin:\n" ..
        "  authenticated: " .. tostring(authenticated_flag == true) .. "\n" ..
        "  complete: false\n" ..
        "  note: 'Heuristic endpoint discovery; not authoritative API documentation.'\n" ..
        "x-prowsetk-beacon:\n" ..
        "  used: " .. tostring(beacon_status.used == true) .. "\n" ..
        "  network-entries: " .. tostring(tonumber(beacon_status.entries) or 0) .. "\n" ..
        "  note: 'Firefox network Flash results are heuristic observations after user consent; not authoritative.'\n" ..
        "x-prowsetk-opencode:\n" ..
        "  used: " .. tostring(opencode_status.used == true) .. "\n" ..
        "  kept: " .. tostring(tonumber(opencode_status.kept) or 0) .. "\n" ..
        "  dropped: " .. tostring(tonumber(opencode_status.dropped) or 0) .. "\n" ..
        "  invented-ignored: " .. tostring(tonumber(opencode_status.invented) or 0) .. "\n" ..
        "  note: 'OpenCode agent cleanup is subtractive and heuristic; " ..
        tostring(opencode_status.note or "disabled") .. ". Kept endpoints are verbatim heuristic discoveries.'\n" ..
        "x-prowsetk-restful:\n" ..
        "  has-post: " .. tostring(restful.has_post == true) .. "\n" ..
        "  is-complete: " .. tostring(restful.is_complete == true) .. "\n" ..
        "  rounds-used: " .. tostring(tonumber(restful.rounds_used) or 0) .. "\n" ..
        "  request-count: " .. tostring(tonumber(restful.request_count) or 0) .. "\n" ..
        "  note: 'restful-resolver iterative resolution; heuristic, not authoritative.'\n"
end

-- Applies the restful-resolver plugin: iteratively probes scraped endpoints
-- through the session until a POST endpoint joins the GET surface, or the
-- round/request budget is exhausted. Never throws: on any failure the seed
-- endpoints are kept and the shortfall is reported in the status table.
local function resolve_until_restful(session, page_url, seed_endpoints, args)
    local status = { has_post = false, is_complete = false,
        rounds_used = 0, request_count = 0 }
    local merged, seen = {}, {}
    local function add(ep)
        if type(ep) ~= "table" or not ep.url or ep.url == "" then return end
        local key = (ep.method or "get") .. "\0" .. ep.url
        if not seen[key] then
            seen[key] = true
            merged[#merged + 1] = ep
        end
    end
    for _, ep in ipairs(seed_endpoints or {}) do add(ep) end
    for _, ep in ipairs(seed_endpoints or {}) do
        if tostring(ep.method or ""):lower() == "post" then
            status.has_post = true
            break
        end
    end
    if restful_resolver == nil then
        status.note = "restful-resolver lua module unavailable; seeds kept"
        return merged, status
    end
    local ok, result = pcall(function()
        return restful_resolver.resolve(session, {
            url = page_url,
            endpoints = seed_endpoints,
            max_rounds = tonumber(args.max_resolve_rounds)
                or tonumber(args.max_api_depth) or 2,
            max_requests = tonumber(args.max_api_requests) or 64,
            require_api_pattern = true,
            redact_secrets = true,
            include_provenance = true,
            infer_schemas = true,
        })
    end)
    if not ok or type(result) ~= "table" then
        status.note = "restful-resolver probe failed; seeds kept"
        return merged, status
    end
    merged, seen = {}, {}
    for _, ep in ipairs(result.endpoints or {}) do add(ep) end
    if no_xcors_only then
        local filtered = {}
        for _, ep in ipairs(merged) do
            if is_booking_tld_url(ep.url) then filtered[#filtered + 1] = ep end
        end
        merged = filtered
    end
    status.has_post = result.has_post == true
    status.is_complete = result.is_complete == true
    status.rounds_used = tonumber(result.rounds_used) or 0
    status.request_count = tonumber(result.request_count) or 0
    return merged, status
end

-- Applies the schema-grabber plugin: reverse-engineers request/response
-- schemas and URL parameters for the merged endpoints so the exported OpenAPI
-- and Postman specs carry req, res, and URL parameters alongside
-- scrape-endpoints discovery. GET response bodies come from bounded
-- host-mediated probes in live runs; offline runs stay fully static (no
-- network). Never throws: on any failure it returns nil and the caller falls
-- back to the plain scrape-endpoints rendering.
local function enrich_with_schemas(session, page_url, seed_endpoints, args, offline)
    if schema_grabber == nil then return nil end
    if type(seed_endpoints) ~= "table" or #seed_endpoints == 0 then return nil end
    local ok, result = pcall(function()
        return schema_grabber.enrich(session, {
            url = page_url,
            endpoints = seed_endpoints,
            require_api_pattern = false,
            probe_get_responses = not offline,
            max_probe_requests = tonumber(args.max_schema_probes) or 32,
            allow_cross_origin = false,
            openapi_version = "3.1.0",
            collection_name = "Discovered API (Booking.com Admin)",
            redact_secrets = true,
            include_provenance = true,
            include_examples = true,
            infer_schemas = true,
        })
    end)
    if not ok or type(result) ~= "table" then return nil end
    return result
end

-- Shared login preparation for the Lua driver and the C++ marionette runner.
-- The caller owns the session; preparation neither exports nor closes it.
local function prepare_booking_session(session, args)
    verbose = verbose_requested(args)
    local url = args.url or DEFAULT_URL
    no_xcors_only = no_xcors_requested(args)
    if type(args.html) == "string" and args.html ~= "" then
        session:load_html(args.html, url)
        return url
    end

    local dotenv = load_dotenv(args.dotenv)
    local captcha_inputs = apply_captcha_inputs(session, args, dotenv)
    local handed_off = false
    if args.assistant_browser_force == true or
       tostring(args.assistant_browser_force or ""):lower() == "true" then
        handed_off = offer_login_assistant_browser(session, url, args,
            "assistant browser launch forced by --assistant_browser_force")
    end

    -- Try imported cookies before requiring credentials. Confirmation checks
    -- the HTTP response and positive DOM evidence after trusted redirects.
    local loaded, initial_body, initial_response =
        probe_authenticated_page(session, url, args.success_selector,
            args.success_beacon, args.success_beacon_type)
    local authenticated_url = loaded and initial_response.final_url or url
    if confirmed_challenge(initial_response) then
        error("booking-dotcom-admin: " .. captcha_handler_note(captcha_inputs, initial_response), 2)
    end
    if not loaded then
        debug_log("imported session did not confirm login; trying credential login")
        local username = env("BOOKING_DOTCOM_USER", dotenv)
        local password = env("BOOKING_DOTCOM_PASS", dotenv)
        if not username or username == "" or not password or password == "" then
            error("booking-dotcom-admin: missing Booking.com credentials", 2)
        end
        local as_token = env("BOOKING_DOTCOM_AS_TOKEN", dotenv)
        local logged_in, login_err = pcall(function()
            login(session, url, username, password, {
                as_token = as_token,
                success_selector = args.success_selector,
                initial_body = initial_body,
                initial_response = initial_response
            })
        end)
        if not logged_in then
            local message = tostring(login_err)
            if message:find("requires browser JavaScript or captcha support", 1, true) then
                error(login_err, 0)
            end
            if message:lower():find("challenge detected", 1, true) or
               message:lower():find("captcha", 1, true) or
               message:find("Human Verification", 1, true) then
                error("booking-dotcom-admin: " .. captcha_handler_note(captcha_inputs), 2)
            end
            error(login_err, 0)
        end
        local load_err
        loaded, load_err = pcall(function()
            authenticated_url = load_authenticated_page(session, url, args.success_selector,
                args.success_beacon, args.success_beacon_type)
        end)
        if not loaded and not handed_off then
            local retry_handoff = offer_login_assistant_browser(
                session, url, args, "login was not confirmed; interactive verification may be required")
            if retry_handoff then
                loaded, load_err = pcall(function()
                    authenticated_url = load_authenticated_page(session, url, args.success_selector,
                        args.success_beacon, args.success_beacon_type)
                end)
            end
        end
        if not loaded then
            local message = tostring(load_err)
            if message:find("requires browser JavaScript or captcha support", 1, true) then
                error(load_err, 0)
            end
            if message:lower():find("challenge detected", 1, true) or
               message:lower():find("captcha", 1, true) or
               message:find("Human Verification", 1, true) then
                error("booking-dotcom-admin: " .. captcha_handler_note(captcha_inputs), 2)
            end
            error(load_err, 0)
        end
    end
    debug_log("authenticated session ready for discovery")
    return authenticated_url
end

-- LuaRuntime::bind_session supplies this managed session. Browser credentials,
-- cookies and the page context stay in the same process for subsequent actions.
function prepare_session(args)
    if not _G.session then error("booking-dotcom-admin: host session required", 2) end
    prepare_booking_session(_G.session, args or {})
    return 0
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
        local authenticated_url = prepare_booking_session(session, args)
        if offline then
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
            if no_xcors_only then
                endpoints = filter_booking_tld(endpoints)
            end
        else
            authenticated_flag = true
            endpoints = crawl(session, authenticated_url, args, true)
        end

        local restful_page = (not offline) and authenticated_url or url
        -- Beacon network Flash results merge before restful resolution so
        -- observed Firefox requests join the GET+POST surface and schema
        -- enrichment like any other heuristic discovery. Disabled by
        -- default; hermetic via beacon_json, live via --flash-on true
        -- (default socket /tmp/beacond.sock unless beacon_socket is set).
        local beacon_status = apply_beacon_network(endpoints, args,
            restful_page, authenticated_flag, offline)
        local restful_status = { has_post = false, is_complete = false,
            rounds_used = 0, request_count = 0 }
        if not offline then
            endpoints, restful_status =
                resolve_until_restful(session, restful_page, endpoints, args)
        else
            for _, ep in ipairs(endpoints or {}) do
                if tostring(ep.method or ""):lower() == "post" then
                    restful_status.has_post = true
                    break
                end
            end
            local has_get = false
            for _, ep in ipairs(endpoints or {}) do
                if tostring(ep.method or ""):lower() == "get" then
                    has_get = true
                    break
                end
            end
            restful_status.is_complete = restful_status.has_post and has_get
        end

        if no_xcors_only then
            endpoints = filter_booking_tld(endpoints or {})
        end
        -- api-only (default on): filter garbage/gunk out of the final specs
        -- so only proper API endpoints are exported. Never throws: when the
        -- plugin filter is unavailable the merged endpoints are kept as-is.
        if api_only_requested(args) and scrape_endpoints.filter_api_endpoints then
            local ok_filter, filtered = pcall(function()
                return scrape_endpoints.filter_api_endpoints(endpoints or {}, {
                    api_only = true,
                })
            end)
            if ok_filter and type(filtered) == "table" then
                endpoints = filtered
            end
        end
        -- OpenCode cleanup (opt-in): subtractive agent pass over the redacted
        -- endpoint list before schema enrichment, so probes and schemas are
        -- computed only for kept endpoints. Hermetic via opencode_json, live
        -- via the lopencode module. Disabled by default; failures keep seeds.
        local opencode_status = { used = false, kept = 0, dropped = 0,
            invented = 0, note = "disabled" }
        endpoints, opencode_status = apply_opencode_cleanup(endpoints or {}, args, offline)
        local enriched = enrich_with_schemas(session, restful_page, endpoints,
            args, offline)
        local yaml, postman_json
        if enriched ~= nil then
            yaml = append_metadata(enriched.openapi_yaml, authenticated_flag,
                restful_status, beacon_status, opencode_status)
            postman_json = enriched.postman_json
        else
            local safe_endpoints = redacted_endpoints(endpoints or {})
            yaml = scrape_endpoints.render_openapi_yaml(safe_endpoints, {
                openapi_version = "3.1.0",
                redact_secrets = true,
                include_provenance = true,
                infer_schemas = true
            })
            yaml = append_metadata(yaml, authenticated_flag, restful_status,
                beacon_status, opencode_status)
            postman_json = scrape_endpoints.render_postman_json(safe_endpoints, {
                collection_name = "Discovered API (Booking.com Admin)",
                redact_secrets = true,
                include_provenance = true
            })
        end
        write_file(output, yaml)
        write_file(postman, postman_json)
        if ebpf_interface and args.ebpf_output and args.ebpf_output ~= "" and
           ebpf_interface.render_observations then
            write_file(args.ebpf_output, ebpf_interface.render_observations(endpoints or {}))
        end

        print("booking-dotcom-admin: wrote heuristic OpenAPI and Postman outputs")
    end)

    pcall(function() session:close() end)
    if not ok then error(err, 0) end
    return 0
end
