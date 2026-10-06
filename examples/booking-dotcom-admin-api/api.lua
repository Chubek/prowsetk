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
local function load_captcha_handler()
    for _, path in ipairs({directory .. '../../plugins/captcha-handler/lua/captcha_handler.lua',
                           directory .. '../../captcha-handler/lua/captcha_handler.lua'}) do
        local file = io.open(path, 'rb')
        if file then file:close(); local ok, mod = pcall(dofile, path); if ok then return mod end end
    end
    return nil
end
local captcha_handler = load_captcha_handler()
local function fail() error('Booking assistant snapshot was not confirmed', 0) end

-- Heuristic anti-bot triage using the captcha-handler Lua helper when present.
-- Detection is never authoritative and never bypasses a challenge: a challenge
-- is solved by the human in the assistant browser, then a fresh snapshot is
-- sent. This emits only a value-free diagnostic and keeps the existing
-- positive-DOM-evidence gate as the export condition.
local function challenge_triage(current)
    if captcha_handler == nil or type(current) ~= 'table' then return end
    local ok, result = pcall(captcha_handler.inspect_response, 'GET', current.url, {
        status = 200, body = current.html or '', headers = {}, final_url = current.url
    })
    if not ok or type(result) ~= 'table' or not result.activated then return end
    io.stderr:write('booking-admin-api: anti-bot challenge inferred (' ..
        tostring(result.category or 'anti-bot') ..
        '); solve it in the assistant browser and resend a fresh snapshot\n')
end

local function evidence(active, args)
    return bridge.login_evidence(active, args)
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

local function run(args, diagnostic)
    args = args or {}
    local offline = args.html ~= nil
    if not offline and args.assistant_browser_enabled == false then fail() end
    local url = args.url or 'https://admin.booking.com/'
    local prowse = require('lprowse')
    if not offline and prowse.url.origin(url) ~= 'https://admin.booking.com' then fail() end
    local wait_ms = args.wait_ms or 30000
    if type(wait_ms) ~= 'number' or wait_ms % 1 ~= 0 or wait_ms < 1 or wait_ms > 30000 then fail() end
    local client, snap, active, browser
    local ok, failure = pcall(function()
        diagnostic.stage = 'IPC module loading'
        bridge.ipc(args.ipc_module)
        if offline then
            snap = {url=url, html=args.html, revision=0, purpose='offline'}
        else
            diagnostic.stage = 'bridge connection'
            local candidate = bridge.connect{descriptor=args.bridge, ipc_module=args.ipc_module}
            if candidate.config.origin ~= 'https://admin.booking.com' then fail() end
            client = candidate
            diagnostic.stage = 'snapshot receipt'
            snap = client:snapshot(0, wait_ms)
            if not snap then fail() end
        end
        diagnostic.stage = 'snapshot parsing'
        active, browser = bridge.load_snapshot(snap)
        -- Retain the Lua-owned Browser for the whole managed Session lifetime.
        local seeds, seen, snapshots, actions, confirmed = {}, {}, 0, 0, 0
        local login_evidence = false
        local function collect(current)
            challenge_triage(current)
            if not offline then
                diagnostic.stage = 'snapshot origin check'
                if prowse.url.origin(current.url) ~= 'https://admin.booking.com' then fail() end
                diagnostic.stage = 'login beacon validation'
                login_evidence = evidence(active, args)
                diagnostic.stage = 'login evidence'
                if not login_evidence then fail() end
            end
            snapshots = snapshots + 1
            diagnostic.stage = 'endpoint extraction'
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
            diagnostic.stage = 'action policy loading'
            for _, action in ipairs(actions_for(args)) do
                diagnostic.actions_started = true
                diagnostic.stage = 'browser action'
                snap = client:act(action, wait_ms)
                actions = actions + 1
                diagnostic.stage = 'snapshot parsing'
                active:load_html(snap.html, snap.url)
                diagnostic.stage = 'browser action'
                if snap.action_status == 'error' then fail() end
                if snap.action_status == 'ok' then confirmed = confirmed + 1 end
                collect(snap)
            end
        end
        diagnostic.stage = 'schema enrichment'
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
            diagnostic.stage = 'OpenAPI export'
            bridge.write(args.output, enriched.openapi_yaml .. metadata, args.ipc_module)
        end
        if args.postman and args.postman ~= '' then
            diagnostic.stage = 'Postman export'
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
    diagnostic.retryable = not ok and diagnostic.stage == 'login evidence' and
        not diagnostic.actions_started and args.retry_login_evidence == true
    if client and not diagnostic.retryable then pcall(function() client:finish() end) end
    if not ok then error(failure, 0) end
    return 0
end

local failure_hints = {
    ['configuration'] = 'check the URL, assistant-browser setting and wait_ms bounds',
    ['IPC module loading'] = 'lquteipc is unavailable; build the selected preset or pass --ipc-module with its lquteipc.so',
    ['bridge connection'] = 'cannot read the private descriptor or connect to its broker; keep qute-assist running',
    ['snapshot receipt'] = 'the broker did not return a snapshot; resend it while qute-assist is running',
    ['snapshot parsing'] = 'captured HTML could not be parsed; check the 16 MiB snapshot and Flatworm node/depth limits',
    ['snapshot origin check'] = 'the captured tab must be on https://admin.booking.com',
    ['login beacon validation'] = 'invalid login beacon; check --success-selector or --success-xpath syntax',
    ['login evidence'] = 'snapshot received, but no account/logout control matched; if already logged in, open the account menu and resend, or set --success-selector / --success-xpath to an authenticated-only DOM control',
    ['action policy loading'] = 'cannot read or validate the actions file',
    ['browser action'] = 'the assistant action failed or did not return a fresh snapshot',
    ['endpoint extraction'] = 'endpoint discovery failed; check the scrape-endpoints Lua helper and extraction bounds',
    ['schema enrichment'] = 'schema enrichment failed; check the schema-grabber Lua helper and snapshot bounds',
    ['OpenAPI export'] = 'cannot write OpenAPI output; check the output parent directory and permissions',
    ['Postman export'] = 'cannot serialize or write Postman output; check the output parent directory and permissions'
}

function main(args)
    local diagnostic = {stage='configuration'}
    local ok, code = pcall(run, args, diagnostic)
    if not ok then
        -- Raw Lua/selector/page/IPC exceptions may contain credentials. Only
        -- trusted stage names and fixed hints cross the diagnostic boundary.
        io.stderr:write('booking-admin-api: ' .. diagnostic.stage .. ' failed: ' ..
            (failure_hints[diagnostic.stage] or 'operation failed') .. '\n')
        if diagnostic.retryable then return 3 end
        return 1
    end
    return code
end
