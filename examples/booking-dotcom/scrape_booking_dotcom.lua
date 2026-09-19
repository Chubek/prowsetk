-- Run from the repository root:
-- prowsetk run booking-dotcom --config examples/booking_dotcom.toml --output build/booking.yaml
-- .env is data, never shell code. Existing process variables take precedence.
--
-- This driver uses ezlogin for form-based authentication and both scrape2oapi
-- and scrape2postman plugins to generate OpenAPI YAML and Postman Collection JSON.
local prowse = require('lprowse')
local source = debug.getinfo(1, 'S').source:gsub('^@', '')
local directory = source:match('^(.*[/\\])') or './'

-- Load ezlogin for authentication
local ezlogin
do
    local ezlogin_path = directory .. '../../plugins/ezlogin/lua/ezlogin.lua'
    local f = loadfile(ezlogin_path)
    if f then ezlogin = f() end
end
assert(ezlogin, 'booking-dotcom: cannot load ezlogin plugin')

-- Load scrape2oapi plugin
local scrape2oapi
do
    local paths = {
        directory .. '../../plugins/scrape2oapi/lua/scrape2oapi.lua',
        directory .. '../plugins/scrape2oapi/lua/scrape2oapi.lua'
    }
    for _, p in ipairs(paths) do
        local f = loadfile(p)
        if f then scrape2oapi = f(); break end
    end
end
assert(scrape2oapi, 'booking-dotcom: cannot load scrape2oapi plugin')

-- Load scrape2postman plugin (requires scrape2oapi to be pre-loaded)
local scrape2postman
do
    -- Pre-load scrape2oapi into package.loaded so scrape2postman can require it
    package.loaded['scrape2oapi'] = scrape2oapi
    local paths = {
        directory .. '../../plugins/scrape2postman/lua/scrape2postman.lua',
        directory .. '../plugins/scrape2postman/lua/scrape2postman.lua'
    }
    for _, p in ipairs(paths) do
        local f = loadfile(p)
        if f then scrape2postman = f(); break end
    end
end
assert(scrape2postman, 'booking-dotcom: cannot load scrape2postman plugin')

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
    local dir = base:match('^(https://.*/)[^/]*$') or root .. '/'
    local joined = dir .. ref
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

-- Merges endpoint arrays, deduplicating by method+path
local function merge_endpoints(endpoint_arrays)
    local seen = {}
    local merged = {}
    for _, arr in ipairs(endpoint_arrays) do
        for _, ep in ipairs(arr) do
            local key = (ep.method or 'get') .. ' ' .. (ep.path or '')
            if not seen[key] then
                seen[key] = #merged + 1
                merged[#merged + 1] = ep
            else
                local existing = merged[seen[key]]
                -- Merge parameters
                for _, p in ipairs(ep.parameters or {}) do
                    local found = false
                    for _, ep2 in ipairs(existing.parameters or {}) do
                        if p == ep2 then found = true; break end
                    end
                    if not found then existing.parameters[#existing.parameters + 1] = p end
                end
                -- Keep higher confidence
                if (ep.confidence or 0) > (existing.confidence or 0) then
                    existing.confidence = ep.confidence
                    existing.url = ep.url
                    existing.source = ep.source
                end
            end
        end
    end
    -- Sort by path then method
    table.sort(merged, function(a, b)
        if a.path ~= b.path then return a.path < b.path end
        return (a.method or '') < (b.method or '')
    end)
    return merged
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
    -- Postman output: derive from output path or use default
    local postman_output = args.postman
    if not postman_output then
        postman_output = output:gsub('%.yaml$', '.postman.json')
        if postman_output == output then
            postman_output = output .. '.postman.json'
        end
    end
    if postman_output:sub(1, 2) == '~/' then
        local home = os.getenv('HOME')
        if not home or home == '' then fail('HOME is unset; supply --output') end
        postman_output = home .. postman_output:sub(2)
    end
    local max_depth = tonumber(args.max_depth) or 2
    local max_pages = tonumber(args.max_pages) or 12

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
        if root_host == host or root_host:match('%.' .. host:gsub('%.', '%%.') .. '$') or
           host:match('%.' .. root_host:gsub('%.', '%%.') .. '$') then
            return true
        end
        if (root_host == 'booking.com' or root_host:match('%.booking%.com$')) and
           (host == 'booking.com' or host:match('%.booking%.com$')) then
            return true
        end
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

    -- Checks if URL is a page (not an API endpoint or asset)
    local function is_page_url(href)
        if not href or href == '' or href:sub(1, 1) == '#' then return false end
        local lower = href:lower()
        -- Skip API paths
        if lower:match('/api/') or lower:match('/v1/') or lower:match('/v2/') or
           lower:match('/graphql') or lower:match('/rest/') or lower:match('/xmlhttp') then
            return false
        end
        -- Skip assets
        if lower:match('%.js$') or lower:match('%.css$') or lower:match('%.png$') or
           lower:match('%.jpg$') or lower:match('%.gif$') or lower:match('%.svg$') or
           lower:match('%.ico$') or lower:match('%.woff') or lower:match('%.json$') then
            return false
        end
        -- Skip logout/signout links
        if lower:match('logout') or lower:match('signout') or lower:match('sign%-out') or
           lower:match('log%-out') then
            return false
        end
        return true
    end

    -- Collects page links for crawling
    local function collect_links(doc, base_url)
        local links = {}
        for _, a in ipairs(doc:query_selector_all('a[href]')) do
            local href = a:attribute('href')
            if is_page_url(href) then
                local full = resolve(base_url, href)
                if trusted(full) then
                    links[#links + 1] = full
                end
            end
        end
        return links
    end

    -- Injects external scripts into document
    local function inject_scripts(doc, page_url, max_scripts)
        local count, skipped, attempted, bytes = 0, 0, 0, 0
        local sources, seen = {}, {}
        for _, script in ipairs(doc:query_selector_all('script[src]')) do
            local valid, target = pcall(resolve, page_url, script:attribute('src'))
            if valid then sources[#sources+1] = target else skipped = skipped + 1 end
        end
        for _, source in ipairs(sources) do
            if trusted(source) and not seen[source] and attempted < (max_scripts or 32) then
                seen[source] = true
                attempted = attempted + 1
                local fetched, body = pcall(request, 'GET', source)
                if fetched and type(body) == 'string' and #body <= 2 * 1024 * 1024 and
                   bytes + #body <= 16 * 1024 * 1024 then
                    bytes = bytes + #body
                    local script = doc:create_element('script')
                    script:set_text(body)
                    doc:root():append_child(script)
                    count = count + 1
                else skipped = skipped + 1 end
            else skipped = skipped + 1 end
        end
        return count, skipped
    end

    -- Scrapes a single page
    local function scrape_page(doc, opts)
        local result = scrape2oapi.scrape(doc, opts)
        return result.endpoints
    end

    local ok, message = pcall(function()
        local is_authenticated = false
        local all_endpoints = {}
        local pages_scraped = 0

        if offline then
            session:load_html(args.html, url)
            -- In offline mode, just scrape the provided HTML without crawling
            stage = 'scraping offline page'
            local doc = session:document()
            local script_count, skipped = inject_scripts(doc, url, 32)
            local page_endpoints = scrape_page(doc, {
                require_api_pattern = false,
                scrape_all_paths = true,
                follow_links = true,
                inspect_scripts = true,
                redact_secrets = true,
                include_provenance = true,
                minimum_confidence = 0,
                output = ''
            })
            all_endpoints[#all_endpoints + 1] = page_endpoints
            pages_scraped = 1
        else
            -- Use ezlogin form-based authentication
            stage = 'authenticating with ezlogin'
            local auth_ok, auth_err = pcall(function()
                ezlogin.configure(session, {
                    mode = 'form',
                    login_url = url,
                    username = username,
                    password = password,
                    username_field = 'username',
                    password_field = 'password',
                    csrf_field = '_csrf'
                })
            end)
            if not auth_ok then
                -- Preserve ezlogin error messages for proper test assertions
                local err_msg = tostring(auth_err)
                if err_msg:find("JavaScript") or err_msg:find("captcha") then
                    fail('login page requires browser JavaScript or captcha support that Flatworm does not yet provide; no OpenAPI file written')
                elseif err_msg:find("no login form") then
                    fail('login form not available or authentication not confirmed; no OpenAPI file written')
                else
                    fail('authentication failed: ' .. err_msg)
                end
            end

            -- Navigate to the admin dashboard after login
            stage = 'loading dashboard'
            local response = session:request('GET', url)
            session:load_html(response.body, response.final_url or url)

            if authenticated() then
                is_authenticated = true
                print('booking-dotcom: login confirmed')
            elseif login_requires_browser_js() then
                fail('login page requires browser JavaScript or captcha support that Flatworm does not yet provide; no OpenAPI file written')
            else
                fail('login not confirmed; no OpenAPI file written')
            end

            -- Recursive page crawling
            stage = 'crawling admin pages'
            local visited = {[session:current_url()] = true}
            local queue = {{url = session:current_url(), depth = 0}}

            while #queue > 0 and pages_scraped < max_pages do
                local item = table.remove(queue, 1)
                local current_url = item.url
                local current_depth = item.depth

                -- Load the page if not already loaded
                if current_url ~= session:current_url() then
                    local response = session:request('GET', current_url)
                    session:load_html(response.body, response.final_url or current_url)
                end

                local doc = session:document()

                -- Inject scripts
                local script_count, skipped = inject_scripts(doc, current_url, 32)

                -- Scrape endpoints from this page
                local page_endpoints = scrape_page(doc, {
                    require_api_pattern = false,
                    scrape_all_paths = true,
                    follow_links = true,
                    inspect_scripts = true,
                    redact_secrets = true,
                    include_provenance = true,
                    minimum_confidence = 0,
                    output = ''
                })
                all_endpoints[#all_endpoints + 1] = page_endpoints
                pages_scraped = pages_scraped + 1

                -- Collect links for crawling (non-offline only)
                if not offline and current_depth < max_depth then
                    local links = collect_links(doc, current_url)
                    for _, link in ipairs(links) do
                        if not visited[link] then
                            visited[link] = true
                            queue[#queue + 1] = {url = link, depth = current_depth + 1}
                        end
                    end
                end
            end
        end

        -- For offline mode, skip the crawl section (already scraped above)
        if not offline then
            -- Crawl already handled in the else block above
        end

        -- Merge all endpoints
        local merged = merge_endpoints(all_endpoints)

        -- Render OpenAPI YAML using core renderer via lprowsext if available
        local yaml
        local ext_ok, ext = pcall(require, 'lprowsext')
        if ext_ok and ext.endpoints and ext.endpoints.render then
            yaml = ext.endpoints.render(merged, {
                include_provenance = true,
                redact_secrets = true,
                infer_schemas = true,
                openapi_version = '3.1.0'
            })
        else
            -- Fallback: use scrape2oapi's render
            yaml = scrape2oapi.render_openapi_yaml(merged, {
                include_provenance = true,
                redact_secrets = true,
                openapi_version = '3.1.0'
            })
        end

        -- Defense in depth for credentials
        for _, secret in ipairs({username, password, encode(username), encode(password)}) do
            if secret ~= '' then yaml = yaml:gsub(secret:gsub('([^%w])', '%%%1'), '[REDACTED]') end
        end

        -- Append booking discovery metadata
        yaml = yaml .. '\nx-booking-discovery:\n  authenticated: ' .. tostring(is_authenticated) ..
            '\n  complete: false\n  pages_scraped: ' .. pages_scraped ..
            '\n  max_depth: ' .. max_depth .. '\n  scope: "Admin panel recursive crawl"\n'

        -- Write OpenAPI YAML
        stage = 'writing OpenAPI'
        local file = io.open(output, 'w')
        if not file then fail('cannot open output; parent directory must exist') end
        file:write(yaml)
        file:close()

        -- Render and write Postman JSON
        stage = 'writing Postman'
        local postman_spec = {
            collection_name = 'Booking.com Admin API',
            redact_secrets = true,
            include_provenance = true
        }
        local postman_json = scrape2postman.render_postman_json(merged, postman_spec)
        file = io.open(postman_output, 'w')
        if not file then fail('cannot open Postman output') end
        file:write(postman_json)
        file:close()

        print(string.format('booking-dotcom: wrote %d endpoints from %d pages', #merged, pages_scraped))
        print('booking-dotcom: OpenAPI: ' .. output)
        print('booking-dotcom: Postman: ' .. postman_output)
        if offline then print('booking-dotcom: offline fixture; authentication was not attempted')
        elseif not is_authenticated then print('booking-dotcom: unauthenticated extraction') end
    end)

    session:close()
    if not ok then
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
