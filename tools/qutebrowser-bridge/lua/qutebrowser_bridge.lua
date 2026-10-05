-- ProwseTk driver-side companion. Native IPC is explicit and independently
-- authenticated; importing a snapshot executes no assistant page scripts.
local source = debug.getinfo(1, 'S').source:gsub('^@', '')
local directory = source:match('^(.*[/\\])') or './'
local json = dofile(directory .. 'json.lua')
local bridge = { json = json }
local function fail() error('qutebrowser bridge operation failed', 0) end

function bridge.ipc(path)
    if package.loaded.lquteipc then return package.loaded.lquteipc end
    if path and path ~= '' then
        local loader = package.loadlib(path, 'luaopen_lquteipc')
        if not loader then fail() end
        package.loaded.lquteipc = loader()
        return package.loaded.lquteipc
    end
    local ok, module = pcall(require, 'lquteipc')
    if ok then return module end
    -- Build-tree convenience; installed callers use package.cpath or an explicit
    -- ipc_module. Native modules are never fetched or built by a driver.
    for _, preset in ipairs({'default', 'debug', 'asan', 'release'}) do
        local loader = package.loadlib(directory .. '../../../build/' .. preset ..
            '/tools/qutebrowser-bridge/lquteipc.so', 'luaopen_lquteipc')
        if loader then
            package.loaded.lquteipc = loader()
            return package.loaded.lquteipc
        end
    end
    fail()
end

local Client = {}
Client.__index = Client
function Client:request(request, timeout)
    request.version, request.token = 1, self.config.token
    local data = self.ipc.exchange(self.config.socket, json.encode(request), timeout or 32000)
    if not data then fail() end
    local reply = json.decode(data)
    if type(reply) ~= 'table' or reply.ok ~= true then fail() end
    return reply
end

function bridge.connect(options)
    options = options or {}
    local ipc = bridge.ipc(options.ipc_module)
    local bytes = ipc.read_private(options.descriptor or os.getenv('PROWSETK_QUTE_BRIDGE') or '')
    if not bytes or #bytes > 16384 then fail() end
    local config = json.decode(bytes)
    if type(config) ~= 'table' or config.version ~= 1 or type(config.socket) ~= 'string' or
       type(config.token) ~= 'string' or not config.token:match('^[0-9a-f]+$') or #config.token ~= 64 or
       type(config.origin) ~= 'string' or require('lprowse').url.origin(config.origin) ~= config.origin then fail() end
    for key in pairs(config) do
        if key ~= 'version' and key ~= 'socket' and key ~= 'token' and key ~= 'origin' then fail() end
    end
    return setmetatable({ config = config, ipc = ipc }, Client)
end

function Client:status() return self:request{op='status'} end
function Client:snapshot(after, wait_ms)
    local result = self:request{op='snapshot', after=after or 0, wait_ms=wait_ms or 30000}
    if result.snapshot == json.null then return nil end
    return result.snapshot
end
function Client:act(action, wait_ms)
    return self:request{op='act', action=action, wait_ms=wait_ms or 30000}.snapshot
end
function Client:finish() return self:request{op='finish'}.ok end

function bridge.load_snapshot(snapshot)
    if type(snapshot) ~= 'table' or type(snapshot.html) ~= 'string' or #snapshot.html > 4 * 1024 * 1024 or
       type(snapshot.url) ~= 'string' then fail() end
    require('lprowse').url.origin(snapshot.url)
    local browser = require('lprowse').browser.new{javascript=false, follow_redirects=false, observe_network=false}
    local active = browser:create_session()
    local ok = pcall(function() active:load_html(snapshot.html, snapshot.url) end)
    if not ok then active:close(); fail() end
    return active, browser
end

function bridge.plugin(name)
    if package.loaded[name] then return package.loaded[name] end
    local paths = {
        directory .. '../../../plugins/' .. name:gsub('_', '-') .. '/lua/' .. name .. '.lua',
        directory .. '../../plugins/' .. name:gsub('_', '-') .. '/' .. name .. '.lua'
    }
    for _, path in ipairs(paths) do
        local file = io.open(path, 'rb')
        if file then
            file:close()
            local module = dofile(path)
            package.loaded[name] = module
            return module
        end
    end
    fail()
end

-- Discovery is document/script-based. Qutebrowser userscripts supply neither
-- historical requests nor response bodies. GET response probing is disabled.
function bridge.scrape(active, options)
    options = options or {}
    return bridge.plugin('scrape_endpoints').scrape(active, {
        follow_links=false, resolve_chain=false, spa_probe=false,
        observe_network=false, inspect_scripts=true, redact_secrets=true,
        include_provenance=true, scrape_all_paths=true,
        api_only=options.api_only ~= false, require_api_pattern=options.api_only ~= false
    })
end

function bridge.enrich(active, endpoints, options)
    options = options or {}
    -- Treat every captured form value as private, including ordinary field
    -- names which a name-based secret detector cannot classify. Sanitize a
    -- detached copy so callers retain their original snapshot for querying.
    local doc = active:document()
    local clean, owner = bridge.load_snapshot{html=doc:html(), url=doc:url()}
    for _, control in ipairs(clean:document():query_selector_all('input, textarea, select, option')) do
        control:remove_attribute('value')
        if control:tag_name():lower() == 'textarea' then control:set_text('') end
    end
    local ok, result = pcall(bridge.plugin('schema_grabber').enrich, clean, {
        endpoints=endpoints, require_api_pattern=options.api_only ~= false,
        probe_get_responses=false, max_probe_requests=0, allow_cross_origin=false,
        redact_secrets=true, include_provenance=true, include_examples=false,
        collection_name=options.collection_name or 'Discovered API (Qutebrowser)'
    })
    clean:close()
    owner = nil
    if not ok then fail() end
    return result
end

function bridge.write(path, data, ipc_module)
    if not bridge.ipc(ipc_module).write_private(path, data) then fail() end
end
return bridge
