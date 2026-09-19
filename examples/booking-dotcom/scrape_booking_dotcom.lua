-- Run from the repository root:
-- prowsetk run booking-dotcom --config examples/booking_dotcom.toml --output build/booking.yaml
-- .env is data, never shell code. Existing process variables take precedence.
local prowse = require('lprowse')
local source = debug.getinfo(1, 'S').source:gsub('^@', '')
local directory = source:match('^(.*[/\\])') or './'
local scrape
for _, p in ipairs({directory .. '../plugins/scrape2oapi/lua/scrape2oapi.lua', directory .. '../plugins/scrape2oai/lua/scrape2oapi.lua', directory .. '../plugins/scrape2oai/lua/scrape2oai.lua'}) do
    local f = loadfile(p)
    if f then scrape = f(); break end
end
assert(scrape, 'booking-dotcom: cannot load scrape plugin')

local function fail(message) error('booking-dotcom: ' .. message, 0) end
local function trim(s) return s:match('^%s*(.-)%s*$') end
local function dotenv(path, required)
    local values = {}
    local file = io.open(path, 'r')
    if not file then
        if required then fail('cannot read dotenv file') end
        return values
    end
    for raw_line in file:lines() do
        local line = trim(raw_line)
        if line ~= '' and line:sub(1, 1) ~= '#' then
            line = line:gsub('^export%s+', '')
            local key, value = line:match('^([%a_][%w_]*)%s*=%s*(.*)$')
            if not key then file:close(); fail('invalid dotenv assignment') end
            local quote = value:sub(1, 1)
            if quote == "'" or quote == '"' then
                local close = 2
                while close <= #value do
                    if value:sub(close, close) == quote then break end
                    if quote == '"' and value:sub(close, close) == '\\' then close = close + 1 end
                    close = close + 1
                end
                local tail = trim(value:sub(close + 1))
                if close > #value or (tail ~= '' and tail:sub(1, 1) ~= '#') then
                    file:close(); fail('invalid quoted dotenv value')
                end
                value = value:sub(2, close - 1)
                if quote == '"' then
                    value = value:gsub('\\([\\"nrt])', {['\\']='\\', ['"']='"', n='\n', r='\r', t='\t'})
                end
            else
                value = trim(value:gsub('%s+#.*$', ''))
            end
            values[key] = value
        end
    end
    file:close()
    return values
end

local function encode(s)
    return (tostring(s):gsub('([^%w%-%_%.%~])', function(c) return string.format('%%%02X', c:byte()) end))
end
local function origin(url)
    return url:match('^(https://[^/?#]+)')
end
local function resolve(base, ref)
    ref = ref or ''
    if ref == '' then return base end
    if ref:match('^https://') then return ref end
    if ref:sub(1, 2) == '//' then return 'https:' .. ref end
    if ref:match('^[%w+.-]+:') then fail('unsupported URL scheme') end
    local root = origin(base)
    if not root then fail('HTTPS is required') end
    if ref:sub(1, 1) == '/' then return root .. ref end
    base = base:gsub('[?#].*$', '')
    if ref:sub(1, 1) == '?' then return base .. ref end
    local directory = base:match('^(https://.*/)[^/]*$') or root .. '/'
    local joined = directory .. ref
    local parts = {}
    for part in joined:sub(#root + 2):gmatch('[^/]+') do
        if part == '..' then table.remove(parts)
        elseif part ~= '.' then parts[#parts + 1] = part end
    end
    return root .. '/' .. table.concat(parts, '/')
end
local function header(response, name)
    for key, value in pairs(response.headers or {}) do
        if key:lower() == name then return value end
    end
end

function main(args)
    args = args or {}
    local offline = args.html ~= nil
    -- Load dotenv BEFORE resolving any credentials; offline runs do not read real secrets.
    local env = offline and {} or dotenv(args.dotenv or '.env', args.dotenv ~= nil)
    local function setting(name)
        local value = os.getenv(name) or env[name]
        if not value or value == '' then fail('missing ' .. name) end
        return value
    end
    local url = offline and 'https://booking.example/' or setting('BOOKING_DOTCOM_URL')
    local username = offline and '' or setting('BOOKING_DOTCOM_USER')
    local password = offline and '' or setting('BOOKING_DOTCOM_PASS')
    if not url:match('^[%w+.-]+:') then url = 'https://' .. url end
    local root = origin(url)
    if not root or root:find('@', 1, true) then fail('a credential-free HTTPS URL is required') end
    local output = args.output or '~/BookingDotcomAdminPanel.yaml'
    if output:sub(1, 2) == '~/' then
        local home = os.getenv('HOME')
        if not home or home == '' then fail('HOME is unset; supply --output') end
        output = home .. output:sub(2)
    end
    local browser = prowse.browser.new({javascript=true, follow_redirects=false, timeout_ms=30000,
                                       observe_network=false})
    local session = browser:create_session()
    local stage = 'initialization'
    local function trusted(target)
        local authority = origin(target)
        if not authority or authority:find('@', 1, true) then return false end
        if authority == root then return true end
        local host = authority:sub(9):lower()
        local root_host = root:sub(9):lower()
        -- Same domain family as the root URL (includes subdomains)
        if root_host == host or root_host:match('%.' .. host:gsub('%.', '%%.') .. '$') or
           host:match('%.' .. root_host:gsub('%.', '%%.') .. '$') then
            return true
        end
        -- Booking.com specific: trust booking.com domain family
        if (root_host == 'booking.com' or root_host:match('%.booking%.com$')) and
           (host == 'booking.com' or host:match('%.booking%.com$')) then
            return true
        end
        -- Booking-owned asset CDNs (for any booking.com root)
        if root_host:match('%.booking%.com$') or root_host == 'booking.com' then
            if host == 'cf.bstatic.com' or host == 'bstatic.com' or host == 'saa.booking.com' then
                return true
            end
        end
        return false
    end
    local function request(method, target, body)
        for _ = 1, 10 do
            if not trusted(target) then fail('redirect or form action is outside the trusted origin') end
            local response = session:request(method, target, {body=body or '',
                headers=body and {['Content-Type']='application/x-www-form-urlencoded'} or {}})
            local status = response.status
            if status == 301 or status == 302 or status == 303 or status == 307 or status == 308 then
                local location = header(response, 'location')
                if not location then fail('redirect has no Location') end
                target = resolve(target, location)
                if status == 303 or ((status == 301 or status == 302) and method == 'POST') then
                    method, body = 'GET', nil
                end
            else
                if status < 200 or status >= 300 then fail('HTTP status ' .. tostring(status)) end
                return response.body, target
            end
        end
        fail('redirect limit reached')
    end
    local function load(method, target, body)
        local html, final = request(method, target, body)
        session:load_html(html, final)
    end
    local function authenticated()
        local doc = session:document()
        if #doc:query_selector_all('input[type="password"]') > 0 then return false end
        return #doc:query_selector_all(args.success_selector or
            'a[href*="logout"], a[href*="signout"], form[action*="logout"], [data-testid="account-menu"]') > 0
    end
    local function login_requires_browser_js()
        local text = (session:document():text() or ''):lower()
        if text:find('please enable javascript', 1, true) or
           text:find('enable javascript in your browser', 1, true) then return true end
        return #session:document():query_selector_all('html.no-js, noscript') > 0
    end
    local ok, message = pcall(function()
        local is_authenticated = false
        if offline then session:load_html(args.html, url)
        else
            stage = 'opening login page'
            load('GET', url)
            local sent_password = false
            local login_attempted = false
            for _ = 1, 3 do
                if sent_password and authenticated() then break end
                local selected, user_input, pass_input
                for _, form in ipairs(session:document():query_selector_all('form')) do
                    local u, p
                    for _, input in ipairs(form:query_selector_all('input')) do
                        local kind = (input:attribute('type') or ''):lower()
                        local name = (input:attribute('name') or ''):lower()
                        if kind == 'password' then p = input
                        elseif kind == 'email' or ((kind == '' or kind == 'text') and
                            (name:find('user', 1, true) or name:find('email', 1, true) or name == 'login')) then u = input end
                    end
                    if p or u then selected, user_input, pass_input = form, u, p; break end
                end
                if not selected then
                    if login_requires_browser_js() then
                        fail('login page requires browser JavaScript or captcha support that Flatworm does not yet provide; no OpenAPI file written')
                    end
                    fail('login form not available or authentication not confirmed; no OpenAPI file written')
                end
                if sent_password then fail('login was not confirmed; refusing to retry the password') end
                if (selected:attribute('method') or ''):lower() ~= 'post' then fail('login form must use POST') end
                local enctype = (selected:attribute('enctype') or ''):lower()
                if enctype ~= '' and enctype ~= 'application/x-www-form-urlencoded' then fail('unsupported login form encoding') end
                if user_input then user_input:set_value(username) end
                if pass_input then pass_input:set_value(password); sent_password = true end
                local fields = {}
                for _, input in ipairs(selected:query_selector_all('input')) do
                    local name = input:attribute('name')
                    local kind = (input:attribute('type') or ''):lower()
                    if name and name ~= '' and not input:has_attribute('disabled') and
                       kind ~= 'submit' and kind ~= 'button' and kind ~= 'file' and kind ~= 'reset' and
                       ((kind ~= 'checkbox' and kind ~= 'radio') or input:has_attribute('checked')) then
                        fields[#fields+1] = encode(name) .. '=' .. encode(input:value())
                    end
                end
                local target = resolve(session:current_url(), selected:attribute('action'))
                stage = 'submitting login form'
                login_attempted = true
                load('POST', target, table.concat(fields, '&'))
            end
            if sent_password and authenticated() then
                is_authenticated = true
                print('booking-dotcom: login confirmed')
            elseif login_attempted and not authenticated() then
                fail('login not confirmed; no OpenAPI file written')
            elseif not is_authenticated then
                fail('login form not available or authentication not confirmed; no OpenAPI file written')
            end
        end
        stage = 'extracting endpoints'
        -- Include same-origin external scripts without executing page code or invoking APIs.
        local script_count, skipped, attempted, script_bytes = 0, 0, 0, 0
        if not offline then
            local page_url = session:current_url()
            local sources, seen = {}, {}
            for _, script in ipairs(session:document():query_selector_all('script[src]')) do
                local valid, target = pcall(resolve, page_url, script:attribute('src'))
                if valid then sources[#sources+1] = target else skipped = skipped + 1 end
            end
            for _, source in ipairs(sources) do
                if trusted(source) and not seen[source] and attempted < 32 then
                    seen[source] = true
                    attempted = attempted + 1
                    local fetched, body = pcall(request, 'GET', source)
                    if fetched and type(body) == 'string' and #body <= 2 * 1024 * 1024 and
                       script_bytes + #body <= 16 * 1024 * 1024 then
                        script_bytes = script_bytes + #body
                        local script = session:document():create_element('script')
                        script:set_text(body)
                        session:document():root():append_child(script)
                        script_count = script_count + 1
                    else skipped = skipped + 1 end
                else skipped = skipped + 1 end
            end
        end
        local result = scrape.scrape(session, {require_api_pattern=false, resolve_chain=false,
            scrape_all_paths=true,
            follow_links=true, inspect_scripts=true, redact_secrets=true, include_provenance=true,
            minimum_confidence=0, output=''})
        if result.endpoint_count == 0 and is_authenticated then fail('no endpoints discovered; no OpenAPI file written') end
        local yaml = result.openapi_yaml
        -- Defense in depth for credentials repeated under arbitrary field names.
        for _, secret in ipairs({username, password, encode(username), encode(password)}) do
            if secret ~= '' then yaml = yaml:gsub(secret:gsub('([^%w])', '%%%1'), '[REDACTED]') end
        end
        yaml = yaml .. '\nx-booking-discovery:\n  authenticated: ' .. tostring(is_authenticated) ..
            '\n  complete: false\n  external-scripts-inspected: ' .. script_count ..
            '\n  external-scripts-skipped: ' .. skipped ..
            '\n  scope: "Current page and same-origin scripts; static heuristic discovery"\n'
        stage = 'writing OpenAPI'
        local file = io.open(output, 'w')
        if not file then fail('cannot open output; parent directory must exist') end
        local written = file:write(yaml)
        local closed = file:close()
        if not written or not closed then fail('cannot finish writing output') end
        print(string.format('booking-dotcom: wrote %d inferred endpoints; completeness is not guaranteed', result.endpoint_count))
        if offline then print('booking-dotcom: offline fixture; authentication was not attempted')
        elseif not is_authenticated then print('booking-dotcom: unauthenticated extraction; login form not available') end
    end)
    session:close()
    if not ok then
        -- Native errors can contain URLs or response data. Only our fixed messages are surfaced.
        if type(message) == 'string' and message:sub(1, 16) == 'booking-dotcom: ' then error(message, 0) end
        local categories = {'HTTPS requires an OpenSSL-enabled build', 'TLS certificate verification failed',
            'TLS handshake failed', 'DNS resolution failed', 'connection failed', 'timed out'}
        for _, category in ipairs(categories) do
            if type(message) == 'string' and message:find(category, 1, true) then
                fail(stage .. ': ' .. category)
            end
        end
        fail(stage .. ' failed (underlying details suppressed to protect credentials)')
    end
    return 0
end
