-- drivers/login.lua
-- The `login` driver: authenticate a session against a login form.
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
--   url       string  required        login page URL
--   username  string  required secret username credential
--   password  string  required secret password credential
--   html      string  optional        operate on this in-memory document
--                                     instead of the network. In this offline
--                                     mode the form is filled and reported but
--                                     not submitted (a dry run).
--   output    path    default "build/login.json"
--
-- Credentials are never written to the output or logged; only the discovered
-- field names and the outcome are recorded.

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

local function write_json(path, value)
    ensure_parent(path)
    local file, err = io.open(path, "w")
    if file == nil then
        error("login: cannot open output file: " .. tostring(path) ..
              " (" .. tostring(err) .. ")", 2)
    end
    file:write(json_encode(value), "\n")
    file:close()
    return true
end

-- ---------------------------------------------------------------------------
-- URL / encoding helpers.
-- ---------------------------------------------------------------------------

local function origin_of(url)
    return tostring(url):match("^(https?://[^/]+)")
end

local function resolve_url(base, reference)
    if reference == nil or reference == "" then
        return base
    end
    if reference:match("^https?://") then
        return reference
    end
    local origin = origin_of(base)
    if origin == nil then
        return reference
    end
    if reference:sub(1, 1) == "/" then
        return origin .. reference
    end
    local directory = tostring(base):match("^(https?://.*/)[^/]*$")
    if directory == nil then
        return origin .. "/" .. reference
    end
    return directory .. reference
end

local function url_encode(value)
    return tostring(value):gsub("([^%w%-%_%.%~])", function(c)
        return string.format("%%%02X", string.byte(c))
    end)
end

-- ---------------------------------------------------------------------------
-- Form discovery and filling.
-- ---------------------------------------------------------------------------

local function lower(value)
    return string.lower(value or "")
end

local function looks_like_username(name)
    return name:find("user", 1, true) ~= nil or
           name:find("email", 1, true) ~= nil or
           name:find("login", 1, true) ~= nil or
           name:find("account", 1, true) ~= nil or
           name:find("name", 1, true) ~= nil
end

-- Returns form, username_input, password_input for the first form that
-- contains a password field.
local function find_login_form(document)
    for _, form in ipairs(document:query_selector_all("form")) do
        local password = nil
        local username = nil
        local fallback = nil
        for _, input in ipairs(form:query_selector_all("input")) do
            local input_type = lower(input:attribute("type"))
            local input_name = lower(input:attribute("name"))
            if input_type == "password" and password == nil then
                password = input
            elseif input_type == "text" or input_type == "email" or
                   input_type == "" then
                if fallback == nil then
                    fallback = input
                end
                if username == nil and looks_like_username(input_name) then
                    username = input
                end
            end
        end
        if password ~= nil then
            return form, username or fallback, password
        end
    end
    return nil, nil, nil
end

local function form_fields(form)
    local fields = {}
    for _, input in ipairs(form:query_selector_all("input")) do
        local name = input:attribute("name")
        if name ~= nil and name ~= "" then
            fields[#fields + 1] = { name = name, value = input:value() }
        end
    end
    return fields
end

local function field_names(fields)
    local names = {}
    for _, field in ipairs(fields) do
        names[#names + 1] = field.name
    end
    return names
end

-- ---------------------------------------------------------------------------
-- Entrypoint.
-- ---------------------------------------------------------------------------

function main(args)
    args = args or {}
    local url = args.url or ""
    local username = args.username or ""
    local password = args.password or ""
    local output = args.output or "build/login.json"
    local html = args.html
    local offline = type(html) == "string" and #html > 0

    if url == "" then
        error("login: missing required argument: url", 2)
    end
    if username == "" then
        error("login: missing required argument: username", 2)
    end
    if password == "" then
        error("login: missing required argument: password", 2)
    end

    local browser = prowse.browser.new({ javascript = false })
    local session = browser:create_session()

    if offline then
        session:load_html(html, url)
    else
        session:navigate(url)
    end

    local document = session:document()
    if document == nil then
        error("login: session has no document", 2)
    end

    local form, username_input, password_input = find_login_form(document)
    if form == nil then
        error("login: no password field found on " .. url, 2)
    end
    if username_input == nil then
        error("login: no username field found on " .. url, 2)
    end

    username_input:set_value(username)
    password_input:set_value(password)
    if username_input:value() ~= username or
       password_input:value() ~= password then
        error("login: failed to populate credentials", 2)
    end

    local method = lower(form:attribute("method"))
    if method ~= "get" and method ~= "post" then
        method = "post"
    end
    local action = resolve_url(session:current_url(), form:attribute("action"))
    local fields = form_fields(form)

    local summary = {
        url = session:current_url(),
        action = action,
        method = string.upper(method),
        username_field = username_input:attribute("name"),
        password_field = password_input:attribute("name"),
        field_names = field_names(fields),
        credentials_redacted = true,
        timestamp = os.date("!%Y-%m-%dT%H:%M:%SZ")
    }

    if offline then
        summary.submitted = false
        summary.note = "offline dry run: credentials were not sent"
        write_json(output, summary)
        print(string.format("login: filled form at %s (dry run)", action))
        session:close()
        return 0
    end

    local parts = {}
    for _, field in ipairs(fields) do
        parts[#parts + 1] = url_encode(field.name) .. "=" ..
                            url_encode(field.value)
    end
    local body = table.concat(parts, "&")

    local response = session:request(method, action, {
        headers = { ["Content-Type"] = "application/x-www-form-urlencoded" },
        body = body
    })

    summary.submitted = true
    summary.status = response.status
    summary.final_url = response.final_url
    write_json(output, summary)

    if response.status >= 400 then
        error("login: authentication request failed with status " ..
              tostring(response.status), 2)
    end

    print(string.format("login: authenticated at %s (status %d)", action,
                        response.status))
    session:close()
    return 0
end
