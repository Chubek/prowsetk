-- Booking.com admin recursive endpoint scraper.
--
-- Live runs read Booking.com credentials from dotenv before looking at the
-- process environment, authenticate through ezlogin, crawl same-origin admin
-- pages, and write OpenAPI plus Postman artifacts under
-- _scraped/booking-dotcom-admin by default. Offline runs pass args.html and do
-- not read credentials. Pass --no-xcors (boolean, default false) to drop any
-- discovered endpoint whose host is outside the booking.com TLD
-- (booking.com or *.booking.com).

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
local captcha_handler = require_first("captcha_handler", "plugins.captcha-handler.lua.captcha_handler")
local restful_resolver = nil
do
    local ok, module = pcall(require_first,
        "restful_resolver", "plugins.restful-resolver.lua.restful_resolver")
    if ok then restful_resolver = module end
end

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

-- Login can fail before scrape2oapi gets a chance to offer its handoff. Keep
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

    io.stderr:write("booking-dotcom-admin: launching " .. tostring(command) ..
        " for " .. tostring(url) .. "\n")
    os.execute(tostring(command) .. " " .. shell_quote(url))
    io.stderr:write("booking-dotcom-admin: press Enter after browser interaction is complete. ")
    io.read("*l")
    local cookie_command = os.getenv("PROWSETK_ASSISTANT_BROWSER_COOKIE_COMMAND") or
        args.assistant_browser_cookie_command or ""
    if cookie_command == "" then
        cookie_command = repo_root .. "/scripts/grab-firefox-cookies.sh"
    end
    if cookie_command ~= "" and cookie_path and cookie_path ~= "" then
        io.stderr:write("booking-dotcom-admin: grabbing Firefox cookies\n")
        os.execute(tostring(cookie_command) .. " " .. shell_quote(cookie_path))
    end
    local imported = 0
    if cookie_path and cookie_path ~= "" and session and
       type(session.import_cookies_json) == "function" then
        local ok, count = pcall(function()
            return session:import_cookies_json(cookie_path)
        end)
        if ok then imported = tonumber(count) or 0 end
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
        response.final_url = current_url
        response.redirect_chain = redirects
        response.captcha = captcha_handler.inspect_response(method, current_url, response)
        local location = response_header(response, "Location")
        if not options.follow_redirects or not is_redirect(response.status) or not location then
            return response.body or "", response
        end
        local next_url = resolve_url(current_url, location)
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
        return ok and type(nodes) == "table" and #nodes > 0
    elseif beacon_type == "css" and type(document.query_selector) == "function" then
        local ok, node = pcall(function() return document:query_selector(value) end)
        return ok and node ~= nil
    elseif beacon_type == "text" and type(document.text) == "function" then
        local ok, text = pcall(function() return document:text() end)
        return ok and tostring(text or ""):lower():find(value:lower(), 1, true) ~= nil
    end
    return false
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
    if confirmed_challenge(response) then return false, body, response end
    local doc = load_page(session, response.final_url, body)
    local valid = response.status >= 200 and response.status < 300 and
        same_origin(url, response.final_url) and
        (authenticated(doc, body, success_selector) or
         beacon_authenticated(doc, success_beacon, success_beacon_type))
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
    local result = scrape2oapi.scrape(session, spec)
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

local function append_metadata(yaml, authenticated_flag, restful)
    restful = restful or {}
    return yaml ..
        "x-prowsetk-booking-admin:\n" ..
        "  authenticated: " .. tostring(authenticated_flag == true) .. "\n" ..
        "  complete: false\n" ..
        "  note: 'Heuristic endpoint discovery; not authoritative API documentation.'\n" ..
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
        no_xcors_only = no_xcors_requested(args)
        local endpoints = nil
        local authenticated_flag = false
        local authenticated_url = url
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
            if no_xcors_only then
                endpoints = filter_booking_tld(endpoints)
            end
        else
            local dotenv = load_dotenv(args.dotenv)
            local captcha_inputs = apply_captcha_inputs(session, args, dotenv)

            -- An explicit force switch is useful when the site has not yet
            -- produced a detectable challenge. The browser is still user
            -- approved, but launch no longer depends on heuristics or a
            -- failed login confirmation.
            if args.assistant_browser_force == true or
               tostring(args.assistant_browser_force or ""):lower() == "true" then
                offer_login_assistant_browser(session, url, args,
                    "assistant browser launch forced by --assistant_browser_force")
            end

            -- Cookies from Prowse.toml and --cookies-json are already in the
            -- session jar; they are not driver arguments. Always try them first.
            local loaded, initial_body, initial_response =
                probe_authenticated_page(session, url, args.success_selector,
                    args.success_beacon, args.success_beacon_type)
            authenticated_url = loaded and initial_response.final_url or url
            if confirmed_challenge(initial_response) then
                error("booking-dotcom-admin: " .. captcha_handler_note(captcha_inputs, initial_response), 2)
            end
            if not loaded then
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
                if not loaded then
                    -- A failed post-login confirmation is exactly the case
                    -- where a user-assisted browser may be required. Give the
                    -- user a chance to complete the interaction, then retry
                    -- the host-mediated confirmation before reporting failure.
                    local handed_off, imported_cookies = offer_login_assistant_browser(
                        session, url, args, tostring(load_err):gsub("^[^:]+:%s*", ""))
                    if handed_off then
                        loaded, load_err = pcall(function()
                            authenticated_url = load_authenticated_page(session, url, args.success_selector,
                                args.success_beacon, args.success_beacon_type)
                        end)
                        if not loaded and imported_cookies == 0 then
                            load_err = tostring(load_err) ..
                                "; Firefox is a separate session; export fresh cookies and rerun with --cookies-json FILE"
                        end
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
            authenticated_flag = true
            endpoints = crawl(session, authenticated_url, args, true)
        end

        local restful_page = (not offline) and authenticated_url or url
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
        local safe_endpoints = redacted_endpoints(endpoints or {})
        local yaml = scrape2oapi.render_openapi_yaml(safe_endpoints, {
            openapi_version = "3.1.0",
            redact_secrets = true,
            include_provenance = true,
            infer_schemas = true
        })
        yaml = append_metadata(yaml, authenticated_flag, restful_status)
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
