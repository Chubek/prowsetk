-- Lua helper for drivers. Credentials are applied to the session only and are
-- never returned or printed. Network access remains host-mediated by Session.
--
-- Modes:
--   "basic" - username/password for HTTP Basic auth
--   "bearer" - token for Bearer auth  
--   "api-key" - token for API key header (default X-API-Key)
--   "custom-header" - name/value for custom header
--   "cookie" - session_cookie string for Cookie header
--   "form" - performs form-based login and sets cookie mode
--   "oauth" - uses op_token + as_token for Booking.com OAuth login
local M = {}

local function base64(value)
    local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    local out = {}
    local bytes = {string.byte(value, 1, #value)}
    for i = 1, #bytes, 3 do
        local a, b, c = bytes[i], bytes[i + 1], bytes[i + 2]
        local n = (a or 0) * 65536 + (b or 0) * 256 + (c or 0)
        out[#out + 1] = alphabet:sub(math.floor(n / 262144) % 64 + 1, math.floor(n / 262144) % 64 + 1)
        out[#out + 1] = alphabet:sub(math.floor(n / 4096) % 64 + 1, math.floor(n / 4096) % 64 + 1)
        out[#out + 1] = b and alphabet:sub(math.floor(n / 64) % 64 + 1, math.floor(n / 64) % 64 + 1) or "="
        out[#out + 1] = c and alphabet:sub(n % 64 + 1, n % 64 + 1) or "="
    end
    return table.concat(out)
end

local function url_encode(s)
    if s == nil then return "" end
    return (tostring(s):gsub("([^%w%-%_%.%~])", function(c)
        return string.format("%%%02X", c:byte())
    end))
end

-- Extracts CSRF token from HTML form (handles attributes in any order)
local function extract_csrf_token(html, field_name)
    field_name = field_name or "_csrf"
    -- Pattern 1: name="..." ... value="..."
    local pattern = 'name=["\']' .. field_name .. '["\'][^>]-value=["\']([^\'\"]*)["\']'
    local token = html:match(pattern)
    if not token then
        -- Pattern 2: value="..." ... name="..."
        pattern = 'value=["\']([^\'\"]*)["\'][^>]-name=["\']' .. field_name .. '["\']'
        token = html:match(pattern)
    end
    if not token then
        -- Pattern 3: input with name only (no value attribute, might be in a hidden input)
        pattern = '<input[^>]-name=["\']' .. field_name .. '["\'][^>]->'
        local input = html:match(pattern)
        if input then
            -- Try to extract value from the matched input
            token = input:match('value=["\']([^\'\"]*)["\']')
        end
    end
    return token or ""
end

-- Extracts session cookie from Set-Cookie header  
local function extract_session_cookie(headers)
    local cookies = {}
    for key, value in pairs(headers or {}) do
        if key:lower() == "set-cookie" then
            for cookie in (value .. ","):gmatch("([^,]+)") do
                local cookie_str = cookie:match("^([^;]+)")
                if cookie_str and cookie_str:match("=") then
                    cookies[#cookies + 1] = cookie_str
                end
            end
        end
    end
    return table.concat(cookies, "; ")
end

-- Performs OAuth-based login for Booking.com (requires op_token and as_token from JS frontend)
function M.oauth_login(session, options)
    options = options or {}
    local login_url = assert(options.login_url, "ezlogin: oauth requires login_url")
    local username = assert(options.username, "ezlogin: oauth requires username")
    local password = assert(options.password, "ezlogin: oauth requires password")
    local op_token = assert(options.op_token, "ezlogin: oauth requires op_token")
    local as_token = options.as_token  -- optional, may be nil for offline testing
    local client_id = options.client_id or "6Z72oHOd36Nn7zk3pirh"
    
    -- Step 1: GET the login page to obtain oauth state if not provided
    local response = session:request("GET", login_url)
    if response.status < 200 or response.status >= 300 then
        error("ezlogin: failed to fetch login page (status " .. tostring(response.status) .. ")")
    end
    
    -- Extract oauth info from page if op_token not provided
    local body = response.body or ""
    local extracted_op = op_token
    local oauth_data = body:match('"oauth":(%b{})')
    if oauth_data and not op_token then
        extracted_op = oauth_data:match('"op_token":"([^"]+)"')
    end
    
    local state = options.state
    local code_challenge = options.code_challenge
    if oauth_data and not state then
        state = oauth_data:match('"state":"([^"]+)"')
    end
    if oauth_data and not code_challenge then
        code_challenge = oauth_data:match('"code_challenge":"([^"]+)"')
    end
    
    -- Step 2: POST to email/login_name endpoint
    local payload = '{"login_name":"' .. username .. '","client_id":"' .. client_id .. '","state":"' .. (state or '') .. '","op_token":"' .. (extracted_op or '') .. '"'
    if as_token then
        payload = payload .. ',"as_token":"' .. as_token .. '"'
    end
    payload = payload .. '}'
    
    local post_response = session:request("POST", login_url, {
        body = payload,
        headers = {
            ["Content-Type"] = "application/json",
            ["X-Requested-With"] = "XMLHttpRequest",
            ["Accept"] = "application/json",
            ["Origin"] = "https://account.booking.com",
            ["Referer"] = "https://account.booking.com/sign-in"
        }
    })
    
    -- Check for human verification block
    if post_response.status == 405 or (post_response.body or ""):find("Human Verification") then
        -- Without as_token, AWS WAF blocks the request
        -- Return partial success if we got a redirect anyway
        if post_response.status >= 300 and post_response.status < 400 then
            local location = (post_response.headers or {})["location"]
            if location then
                local final_response = session:request("GET", location)
                local session_cookie = extract_session_cookie(final_response.headers)
                if session_cookie ~= "" then
                    session:set_header("Cookie", session_cookie)
                    return true, session_cookie
                end
            end
        end
        error("ezlogin: oauth login blocked by human verification (as_token required for automated login)")
    end
    
    -- Step 3: Extract session cookies from redirect response
    local session_cookie = extract_session_cookie(post_response.headers)
    if session_cookie == "" and post_response.status >= 300 and post_response.status < 400 then
        local location = (post_response.headers or {})["location"]
        if location then
            local final_response = session:request("GET", location)
            session_cookie = extract_session_cookie(final_response.headers)
        end
    end
    
    if session_cookie ~= "" then
        session:set_header("Cookie", session_cookie)
        return true, session_cookie
    end
    
    -- Check for success indicators in response body
    local body_lower = (post_response.body or ""):lower()
    if body_lower:find("logout") or body_lower:find("dashboard") or body_lower:find("extranet") then
        return true, ""
    end
    
    return false, "oauth login failed - no session cookie obtained"
end

-- Performs form-based login similar to the original driver
-- Collects all form fields and submits them with credentials
function M.form_login(session, options)
    options = options or {}
    local login_url = assert(options.login_url, "ezlogin: form requires login_url")
    local username = assert(options.username, "ezlogin: form requires username")
    local password = assert(options.password, "ezlogin: form requires password")
    local username_field = options.username_field or "username"
    local password_field = options.password_field or "password"
    local csrf_field = options.csrf_field or "_csrf"
    
    local current_url = login_url
    local sent_password = false
    local max_steps = 5
    local step = 0
    
    while step < max_steps do
        step = step + 1
        
        -- GET the current page
        local response = session:request("GET", current_url)
        if response.status < 200 or response.status >= 300 then
            error("ezlogin: failed to fetch page (status " .. tostring(response.status) .. ")")
        end
        
        local body_lower = (response.body or ""):lower()
        
        -- Check for JavaScript requirement
        if body_lower:find("please enable javascript") or body_lower:find("enable javascript in your browser") then
            error("ezlogin: login page requires JavaScript that is not available")
        end
        if response.body:match('<html[^>]-class=["\'][^"\']-*no%-js') or response.body:match('<noscript>') then
            if not response.body:match('<form') then
                error("ezlogin: login page requires JavaScript or has no form")
            end
        end
        
        -- Find the login form (form with password field or username/email field)
        local form_match = response.body:match('<form([^>]*)>')
        if not form_match then
            if body_lower:find("logout") or body_lower:find("sign out") or body_lower:find("log out") then
                return true, ""  -- Already authenticated
            end
            error("ezlogin: no login form found on page")
        end
        
        -- Get form method
        local method = form_match:match('method=["\']([^"\']*)["\']') or "get"
        if method:lower() ~= "post" then
            error("ezlogin: login form must use POST method (found: " .. method:lower() .. ")")
        end
        
        -- Get form action URL
        local action = form_match:match('action=["\']([^"\']*)["\']') or ""
        local post_url = current_url
        if action ~= "" then
            if action:match('^https?://') then
                post_url = action
            elseif action:sub(1, 1) == '/' then
                local origin = current_url:match('^(https?://[^/]+)')
                post_url = origin .. action
            else
                local base = current_url:match('^(https://[^/]+/[^?#]*)') or current_url:match('^(https://[^/]+/)')
                post_url = (base or current_url) .. action
            end
            -- Validate same-origin
            local post_origin = post_url:match('^(https?://[^/]+)')
            local current_origin = current_url:match('^(https?://[^/]+)')
            if post_origin and current_origin and post_origin ~= current_origin then
                error("ezlogin: form action is outside trusted origin (" .. post_origin .. " vs " .. current_origin .. ")")
            end
        end
        
        -- Collect all form fields
        local fields = {}
        local field_order = {}
        for input in response.body:gmatch('<input([^>]*)>') do
            local input_type = input:match('type=["\']([^"\']*)["\']') or "text"
            local input_name = input:match('name=["\']([^"\']*)["\']')
            local input_value = input:match('value=["\']([^"\']*)["\']') or ""
            
            -- Skip certain field types
            local skip_type = input_type:lower()
            if skip_type == "submit" or skip_type == "button" or skip_type == "file" or skip_type == "reset" then
                -- Skip
            elseif skip_type == "checkbox" or skip_type == "radio" then
                -- Only include if checked
                if input:match('checked') then
                    if input_name and input_name ~= "" then
                        table.insert(fields, {name = input_name, value = input_value})
                        table.insert(field_order, input_name)
                    end
                end
            elseif input_name and input_name ~= "" then
                -- Text, email, password, hidden, etc.
                table.insert(fields, {name = input_name, value = input_value, type = input_type:lower()})
                table.insert(field_order, input_name)
            end
        end
        
        -- Also collect textarea and select fields
        for textarea in response.body:gmatch('<textarea([^>]*)name=["\']([^"\']*)["\'][^>]*>([^<]*)</textarea>') do
            local name = textarea:match('name=["\']([^"\']*)["\']')
            if name and name ~= "" then
                table.insert(fields, {name = name, value = ""})  -- Textarea value is between tags, handle separately
            end
        end
        
        -- Build form data, filling in credentials for appropriate fields
        local form_parts = {}
        local has_username_field = false
        local has_password_field = false
        
        for _, field in ipairs(fields) do
            local name_lower = field.name:lower()
            local is_username_field = name_lower == username_field:lower() 
                or name_lower == "email" 
                or name_lower == "login"
                or field.type == "email"
            local is_password_field = field.type == "password" or name_lower == "password"
            
            local value = field.value
            if is_username_field and not has_username_field then
                value = username
                has_username_field = true
            elseif is_password_field and not has_password_field and not sent_password then
                value = password
                has_password_field = true
                sent_password = true
            end
            
            table.insert(form_parts, url_encode(field.name) .. "=" .. url_encode(value))
        end
        
        -- Add CSRF token if found but not already in form
        local csrf_token = extract_csrf_token(response.body, csrf_field)
        if csrf_token and csrf_token ~= "" then
            local has_csrf = false
            for _, f in ipairs(fields) do if f.name == csrf_field then has_csrf = true; break end end
            if not has_csrf then
                table.insert(form_parts, url_encode(csrf_field) .. "=" .. url_encode(csrf_token))
            end
        end
        
        local form_data = table.concat(form_parts, "&")
        if form_data == "" then
            error("ezlogin: no form fields found")
        end
        
        -- POST the form
        local post_response = session:request("POST", post_url, {
            body = form_data,
            headers = {["Content-Type"] = "application/x-www-form-urlencoded"}
        })
        
        -- Handle response
        if post_response.status >= 300 and post_response.status < 400 then
            local location = (post_response.headers or {})["location"]
            if location then
                local final_response = session:request("GET", location)
                local session_cookie = extract_session_cookie(final_response.headers)
                if session_cookie ~= "" then
                    session:set_header("Cookie", session_cookie)
                    return true, session_cookie
                end
                local final_body_lower = (final_response.body or ""):lower()
                if final_body_lower:find("logout") or final_body_lower:find("dashboard") or final_body_lower:find("sign out") then
                    return true, ""
                end
                if sent_password then
                    return false, "login not confirmed after password submitted"
                end
                current_url = location
            else
                current_url = post_url
            end
        else
            local resp_body_lower = (post_response.body or ""):lower()
            if resp_body_lower:find("logout") or resp_body_lower:find("dashboard") or resp_body_lower:find("sign out") then
                return true, ""
            end
            if resp_body_lower:match('<form') and (resp_body_lower:match('password') or resp_body_lower:match('type=["\']email')) then
                if sent_password then
                    return false, "login not confirmed - still seeing login form"
                end
                current_url = post_url
            else
                local session_cookie = extract_session_cookie(post_response.headers)
                if session_cookie ~= "" then
                    session:set_header("Cookie", session_cookie)
                    return true, session_cookie
                end
                if sent_password then
                    return false, "login not confirmed"
                else
                    return false, "unexpected response"
                end
            end
        end
    end
    
    return false, "login flow exceeded maximum steps"
end

function M.configure(session, options)
    options = options or {}
    local mode = options.mode or "bearer"
    if mode == "basic" then
        assert(options.username and options.password, "ezlogin: basic requires username/password")
        local raw = options.username .. ":" .. options.password
        session:set_header("Authorization", "Basic " .. base64(raw))
    elseif mode == "bearer" then
        assert(options.token, "ezlogin: bearer requires token")
        session:set_header("Authorization", "Bearer " .. options.token)
    elseif mode == "api-key" then
        assert(options.token, "ezlogin: api-key requires token")
        session:set_header(options.name or "X-API-Key", options.token)
    elseif mode == "custom-header" then
        assert(options.name and options.value, "ezlogin: custom-header requires name/value")
        session:set_header(options.name, options.value)
    elseif mode == "cookie" then
        assert(options.session_cookie, "ezlogin: cookie requires session_cookie")
        session:set_header("Cookie", options.session_cookie)
    elseif mode == "form" then
        -- Perform form login and automatically switch to cookie mode
        local ok, err = M.form_login(session, options)
        if not ok then
            error("ezlogin: form login failed: " .. tostring(err))
        end
    elseif mode == "oauth" then
        -- Perform OAuth-based login for Booking.com (requires op_token, optionally as_token)
        local ok, err = M.oauth_login(session, options)
        if not ok then
            error("ezlogin: oauth login failed: " .. tostring(err))
        end
    else
        error("ezlogin: unsupported mode " .. tostring(mode), 2)
    end
    return true
end

return M
