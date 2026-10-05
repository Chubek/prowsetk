-- main(args) is shared by one-shot scrape and two-way marionette workflows.
local source = debug.getinfo(1, 'S').source:gsub('^@', '')
local directory = source:match('^(.*[/\\])') or './'
local function load_bridge()
    for _, path in ipairs({directory .. '../../tools/qutebrowser-bridge/lua/qutebrowser_bridge.lua',
                           directory .. '../../qutebrowser-bridge/lua/qutebrowser_bridge.lua'}) do
        local file = io.open(path, 'rb')
        if file then file:close(); return dofile(path) end
    end
    error('qutebrowser bridge module unavailable', 0)
end
local bridge = load_bridge()
local function fail() error('Booking assistant snapshot was not confirmed', 0) end

local function evidence(active, args)
    local doc = active:document()
    local nodes
    if args.success_xpath and args.success_xpath ~= '' then
        nodes = doc:xpath(args.success_xpath)
    else
        nodes = doc:query_selector_all(args.success_selector or
            "a[href*='logout'], [data-testid='account-menu']")
    end
    for _, node in ipairs(nodes) do
        local tag = node:tag_name():lower()
        if tag ~= 'script' and tag ~= 'style' and tag ~= 'meta' then return true end
    end
    return false
end

local function actions_for(args)
    if not args.actions_file or args.actions_file == '' then return {} end
    local file = assert(io.open(args.actions_file, 'rb'))
    local bytes = file:read(262145)
    file:close()
    if #bytes > 262144 then fail() end
    local policy = bridge.json.decode(bytes)
    if type(policy) ~= 'table' or policy.version ~= 1 or type(policy.actions) ~= 'table' or
       #policy.actions > 32 then fail() end
    for key in pairs(policy) do if key ~= 'version' and key ~= 'actions' then fail() end end
    for key in pairs(policy.actions) do
        if type(key) ~= 'number' or key < 1 or key > #policy.actions or key % 1 ~= 0 then fail() end
    end
    return policy.actions
end

local function run(args)
    args = args or {}
    local offline = args.html ~= nil
    if not offline and args.assistant_browser_enabled == false then fail() end
    local url = args.url or 'https://admin.booking.com/'
    local prowse = require('lprowse')
    if not offline and prowse.url.origin(url) ~= 'https://admin.booking.com' then fail() end
    local wait_ms = args.wait_ms or 30000
    if type(wait_ms) ~= 'number' or wait_ms % 1 ~= 0 or wait_ms < 1 or wait_ms > 30000 then fail() end
    local client, snap, active, browser
    local ok = pcall(function()
        if offline then
            snap = {url=url, html=args.html, revision=0, purpose='offline'}
        else
            local candidate = bridge.connect{descriptor=args.bridge, ipc_module=args.ipc_module}
            if candidate.config.origin ~= 'https://admin.booking.com' then fail() end
            client = candidate
            snap = client:snapshot(0, wait_ms)
            if not snap then fail() end
        end
        active, browser = bridge.load_snapshot(snap)
        -- Retain the Lua-owned Browser for the whole managed Session lifetime.
        local seeds, seen, snapshots, actions, confirmed = {}, {}, 0, 0, 0
        local login_evidence = false
        local function collect(current)
            if not offline then
                if prowse.url.origin(current.url) ~= 'https://admin.booking.com' then fail() end
                login_evidence = evidence(active, args)
                if not login_evidence then fail() end
            end
            snapshots = snapshots + 1
            local result = bridge.scrape(active, {api_only=args.api_only})
            for _, ep in ipairs(result.endpoints) do
                local valid, ep_origin = pcall(prowse.url.origin, ep.url)
                local key = tostring(ep.method):lower() .. ' ' .. tostring(ep.url)
                if valid and ep_origin == prowse.url.origin(url) and not seen[key] then
                    if #seeds >= 10000 then fail() end
                    seen[key], seeds[#seeds + 1] = true, ep
                end
            end
        end
        collect(snap)
        if not offline then
            for _, action in ipairs(actions_for(args)) do
                snap = client:act(action, wait_ms)
                actions = actions + 1
                active:load_html(snap.html, snap.url)
                if snap.action_status == 'error' then fail() end
                if snap.action_status == 'ok' then confirmed = confirmed + 1 end
                collect(snap)
            end
        end
        local enriched = bridge.enrich(active, seeds, {
            api_only=args.api_only, collection_name='Discovered API (Booking.com Admin / Qutebrowser)'
        })
        local metadata = '\nx-prowsetk-qutebrowser:\n' ..
            '  used: ' .. tostring(not offline) .. '\n' ..
            '  offline: ' .. tostring(offline) .. '\n' ..
            '  login-evidence: ' .. tostring(login_evidence) .. '\n' ..
            '  authentication-verified: false\n' ..
            '  complete: false\n' ..
            '  snapshots: ' .. snapshots .. '\n' ..
            '  actions-dispatched: ' .. actions .. '\n' ..
            '  actions-confirmed: ' .. confirmed .. '\n' ..
            "  source: 'Assistant DOM and inline scripts; heuristic discovery.'\n" ..
            "  response-probes: false\n"
        if args.output and args.output ~= '' then
            bridge.write(args.output, enriched.openapi_yaml .. metadata, args.ipc_module)
        end
        if args.postman and args.postman ~= '' then
            local postman = bridge.json.decode(enriched.postman_json)
            postman['x-prowsetk-qutebrowser'] = {
                used=not offline, offline=offline, complete=false,
                login_evidence=login_evidence, authentication_verified=false,
                snapshots=snapshots, actions_dispatched=actions, actions_confirmed=confirmed,
                response_probes=false
            }
            bridge.write(args.postman, bridge.json.encode(postman) .. '\n', args.ipc_module)
        end
    end)
    if active then pcall(function() active:close() end) end
    browser = nil
    if client then pcall(function() client:finish() end) end
    if not ok then fail() end
    return 0
end

function main(args)
    local ok, code = pcall(run, args)
    if not ok then
        io.stderr:write('booking-admin-api: operation failed\n')
        return 1
    end
    return code
end
