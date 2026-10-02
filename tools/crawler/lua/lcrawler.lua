-- A bounded, same-origin breadth-first crawler. The host owns transport policy,
-- cookies, limits and output commits; Lua owns the crawl and scraping workflow.
local prowse = require('lprowse')
local pdql = require('lpdql')
local ezlogin = require('ezlogin')
local scraper = require('scrape_endpoints')
local M = {}
local results = {}

local function quote(value)
    return '"' .. tostring(value or ''):gsub('[%z\1-\31\\"]', function(c)
        if c == '"' or c == '\\' then return '\\' .. c end
        return string.format('\\u%04x', c:byte())
    end) .. '"'
end
local function lines(value)
    local result = {}
    for line in (value or ''):gmatch('[^\r\n]+') do result[#result + 1] = line end
    return result
end
local function glob(pattern, value)
    local escaped = pattern:gsub('([%%%^%$%(%)%.%[%]%+%-])', '%%%1')
        :gsub('%*', '.*'):gsub('%?', '.')
    return value:match('^' .. escaped .. '$') ~= nil
end
local function read_dotenv(path)
    local env = {}
    if not path or path == '' then return env end
    local file = io.open(path, 'rb')
    if not file then return env end
    local bytes = file:read(65537); file:close()
    assert(#bytes <= 65536, 'dotenv exceeds limit')
    for original in (bytes .. '\n'):gmatch('(.-)\n') do
        local line = original:gsub('^%s*export%s+', '')
        local name, value = line:match('^%s*([%a_][%w_]*)%s*=%s*(.-)%s*$')
        if name then
            if value:sub(1, 1) == '"' and value:sub(-1) == '"' then
                value = value:sub(2, -2):gsub('\\([nrt\\"])', {n='\n', r='\r', t='\t', ['\\']='\\', ['"']='"'})
            elseif value:sub(1, 1) == "'" and value:sub(-1) == "'" then value = value:sub(2, -2)
            else value = value:gsub('%s+#.*$', '') end
            env[name] = value
        end
    end
    return env
end
local function fetch(session, url)
    local response = session:request('GET', url)
    if response.status < 200 or response.status >= 300 then return nil end
    local final = response.final_url or url
    if final == '' then final = url end
    local content_type = ''
    for key, value in pairs(response.headers or {}) do
        if key:lower() == 'content-type' then content_type = value:lower() end
    end
    if content_type ~= '' and not content_type:find('html', 1, true) then return nil end
    session:load_html(response.body or '', final)
    return session:document()
end
local function authenticate(session, args, trust)
    local mode = args.login_mode or 'none'
    if mode == 'none' then return true end
    local marker = args.success_selector or ''
    if mode == 'form' or mode == 'oauth' then
        assert(marker ~= '', 'form/OAuth login requires success_selector')
        local ok, doc = pcall(fetch, session, args.url)
        if ok and doc and doc:query_selector(marker) then return true end
    end
    local env = read_dotenv(args.dotenv)
    local function secret(name) return env[name or ''] or os.getenv(name or '') end
    local options = {
        mode = mode, login_url = args.login_url or args.url,
        username = secret(args.username_env), password = secret(args.password_env),
        token = secret(args.token_env), value = secret(args.value_env),
        session_cookie = secret(args.cookie_env), as_token = secret(args.as_token_env),
        name = args.header_name, username_field = args.username_field,
        password_field = args.password_field, csrf_field = args.csrf_field,
        trusted = trust
    }
    local ok = pcall(ezlogin.configure, session, options)
    if not ok then return false end
    if mode == 'form' or mode == 'oauth' then
        -- ezlogin has already applied Set-Cookie through Session. Use the scoped
        -- jar rather than retaining an unscoped raw Cookie request header.
        session:clear_headers()
        local loaded, doc = pcall(fetch, session, args.url)
        return loaded and doc ~= nil and doc:query_selector(marker) ~= nil
    end
    return true
end
local function robots(session, root, enabled)
    if not enabled then return function() return true end end
    local ok, response = pcall(session.request, session, 'GET', root .. '/robots.txt')
    if not ok or response.status == 401 or response.status == 403 or response.status >= 500 then
        return function() return false end
    end
    if response.status ~= 200 then return function() return true end end
    local rules, agents, group_rules = {}, {}, {}
    local function commit()
        for _, agent in ipairs(agents) do
            if agent == '*' or agent == 'prowsetk' then
                for _, rule in ipairs(group_rules) do rules[#rules + 1] = rule end
                break
            end
        end
        agents, group_rules = {}, {}
    end
    for line in ((response.body or '') .. '\n'):gmatch('(.-)\n') do
        local name, value = line:gsub('#.*$', ''):match('^%s*([%w%-]+)%s*:%s*(.-)%s*$')
        if name then
            name = name:lower()
            if name == 'user-agent' then
                if #group_rules > 0 then commit() end
                agents[#agents + 1] = value:lower()
            elseif (name == 'allow' or name == 'disallow') and value ~= '' then
                group_rules[#group_rules + 1] = {value=value, allow=name == 'allow'}
            end
        end
    end
    commit()
    return function(url)
        local path = url:sub(#root + 1)
        local longest, allowed = -1, true
        for _, rule in ipairs(rules) do
            local pattern = rule.value
            if pattern:sub(-1) == '$' then pattern = pattern:sub(1, -2) else pattern = pattern .. '*' end
            if glob(pattern, path) and (#rule.value > longest or (#rule.value == longest and rule.allow)) then
                longest, allowed = #rule.value, rule.allow
            end
        end
        return allowed
    end
end

function M.run(session, args)
    args = args or {}
    results = {}
    local url = assert(args.url, 'crawler requires url')
    local root = prowse.url.origin(url)
    local query = args.query or 'select tag, text from <h*>'
    pdql.validate(query)
    local max_pages, max_depth = tonumber(args.max_pages) or 100, tonumber(args.max_depth) or 2
    local max_frontier = tonumber(args.max_frontier) or 1000
    assert(max_pages >= 1 and max_pages <= 10000 and max_depth >= 0 and max_depth <= 32 and
        max_frontier >= 1 and max_frontier <= 100000, 'crawler bounds invalid')
    local trusted = {[root]=true}
    for _, value in ipairs(lines(args.trusted_origins)) do trusted[prowse.url.origin(value)] = true end
    local function trust(value)
        local ok, target = pcall(prowse.url.origin, value)
        return ok and trusted[target] == true
    end
    local offline = args.html ~= nil
    local challenged = false
    session:on('anti_bot_detected', function() challenged = true end)
    if not offline and not authenticate(session, args, trust) then return 3 end
    challenged = false
    local permitted = robots(session, root, not offline and args.respect_robots ~= false)
    local blocked = lines(args.blocked_paths or '*/logout*\n*/signout*\n*/delete*')
    local function admissible(value)
        local ok, target = pcall(prowse.url.origin, value)
        if not ok or target ~= root then return false end
        local path = value:sub(#root + 1)
        for _, pattern in ipairs(blocked) do if glob(pattern, path) then return false end end
        return permitted(value)
    end
    local start = prowse.url.normalize(url)
    -- Authentication may have landed on a dashboard rather than the seed URL.
    if not offline and (args.login_mode == 'form' or args.login_mode == 'oauth') then
        local current = session:current_url()
        if admissible(current) then start = prowse.url.normalize(current) end
    end
    local frontier, seen, visited = {{url=start, depth=0}}, {[start]=true}, {}
    local records, endpoints, endpoint_seen = {}, {}, {}
    local cursor, attempts, failures, truncated = 1, 0, 0, false
    local output_bytes = 0
    while cursor <= #frontier and attempts < max_pages do
        local item = frontier[cursor]; cursor = cursor + 1; attempts = attempts + 1
        local ok, document
        if offline then session:load_html(args.html, url); ok, document = true, session:document()
        elseif admissible(item.url) then ok, document = pcall(fetch, session, item.url) end
        if challenged and not offline then return 3 end
        if ok and document then
            local final = prowse.url.normalize(session:current_url())
            if prowse.url.origin(final) ~= root then return 1 end
            if not visited[final] then
                visited[final] = true
                if type(on_page) == 'function' then on_page(session, args) end
                local data = pdql.query(session, query)
                local extraction = scraper.scrape(session, {
                    follow_links=false, resolve_chain=false, spa_probe=false,
                    scrape_all_paths=true, api_only=args.api_only ~= false,
                    observe_network=true, inspect_scripts=true, redact_secrets=true
                })
                for _, ep in ipairs(extraction.endpoints or {}) do
                    local key = tostring(ep.method) .. ' ' .. tostring(ep.path)
                    local index = endpoint_seen[key]
                    if not index then
                        assert(#endpoints < 10000, 'endpoint limit exceeded')
                        endpoints[#endpoints + 1] = ep; endpoint_seen[key] = #endpoints
                    else
                        local previous = endpoints[index]
                        local parameters, known = {}, {}
                        for _, previous_record in ipairs({previous, ep}) do
                            for _, name in ipairs(previous_record.parameters or {}) do
                                if not known[name] then parameters[#parameters + 1] = name; known[name] = true end
                            end
                        end
                        if (ep.confidence or 0) > (previous.confidence or 0) then endpoints[index] = ep end
                        table.sort(parameters); endpoints[index].parameters = parameters
                    end
                end
                local record = '{"url":' .. quote(prowse.url.redact(final)) ..
                    ',"title":' .. quote(document:title()) .. ',"depth":' .. item.depth .. ',"data":' .. data .. '}'
                output_bytes = output_bytes + #record + 1
                assert(output_bytes <= 16 * 1024 * 1024, 'crawl output exceeds limit')
                records[#records + 1] = record
                if not offline then
                    for _, link in ipairs(document:links()) do
                        local resolved, target = pcall(prowse.url.resolve, final, link:attribute('href'))
                        if resolved then
                            target = prowse.url.normalize(target)
                            if not seen[target] and admissible(target) then
                                if item.depth < max_depth and #frontier < max_frontier then
                                    seen[target] = true; frontier[#frontier + 1] = {url=target, depth=item.depth + 1}
                                else truncated = true end
                            end
                        end
                    end
                end
            end
        else failures = failures + 1 end
        if offline then break end
    end
    if cursor <= #frontier then truncated = true end
    table.sort(endpoints, function(a,b) return tostring(a.method)..' '..tostring(a.url) < tostring(b.method)..' '..tostring(b.url) end)
    results.pages = table.concat(records, '\n') .. (#records > 0 and '\n' or '')
    assert(#results.pages <= 16 * 1024 * 1024, 'crawl output exceeds limit')
    results.openapi = scraper.render_openapi_yaml(endpoints, {redact_secrets=true, api_only=args.api_only ~= false})
    results.postman = scraper.render_postman_json(endpoints, {redact_secrets=true, api_only=args.api_only ~= false})
    assert(#results.openapi <= 16 * 1024 * 1024 and #results.postman <= 16 * 1024 * 1024, 'specification output exceeds limit')
    results.summary = '{"pages":' .. #records .. ',"attempts":' .. attempts .. ',"failures":' .. failures ..
        ',"truncated":' .. tostring(truncated) .. ',"complete":false,"offline":' .. tostring(offline) .. '}'
    return #records > 0 and 0 or 1
end
function __crawler_result(args) return results[args.kind] or '' end
M.quote = quote
return M
