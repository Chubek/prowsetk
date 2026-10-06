-- Trusted host Lua console. Page/model data never becomes executable Lua.
local source = debug.getinfo(1, 'S').source:gsub('^@', '')
local directory = source:match('^(.*[/\\])') or './'
local bridge = dofile(directory .. 'qutebrowser_bridge.lua')
local prowse = require('lprowse')
local Console = {}
Console.__index = Console
local function fail() error('qute console operation failed', 0) end
local function safe(value, limit)
    return tostring(value or ''):gsub('[%z\1-\8\11-\31\127]', '?'):sub(1, limit or 160)
end
local function bare(value) return safe(tostring(value or ''):gsub('[?#].*$', ''), 512) end
local function display(value)
    if value == nil then return end
    if type(value) == 'table' then
        local ok, encoded = pcall(bridge.json.encode, value)
        if ok then print(safe(encoded, 32768)); return end
        print('<Lua table>'); return
    end
    print(safe(value, 8192))
end

function Console:start(options)
    self.options = options
    self.stage, self.revision, self.snapshots = 'bridge connection', 0, 0
    self.endpoints, self.schemas, self.seen, self.indexed = {}, {}, {}, {}
    self.client = bridge.connect{descriptor=options.bridge, ipc_module=options.ipc_module}
    self.origin = self.client.config.origin
    self.stage = 'snapshot receipt'
    local snap = self.client:snapshot(0, 0)
    if snap then self:install(snap) end
    print('Qutebrowser Lua console ready. :help lists orders; Lua runs in this persistent host session.')
    if not snap then print('Attach ptk-qute-marionette in your logged-in tab, then enter :capture.') end
end

function Console:install(snap)
    self.stage = 'snapshot parsing'
    -- Even a rejected capture can describe a changed external tab. Invalidate
    -- old targets and login evidence before loading the replacement snapshot.
    self.indexed, self.current = {}, false
    self.login_evidence, self.confirmed = false, false
    if self.snapshots >= 2048 then fail() end
    if prowse.url.origin(snap.url) ~= self.origin then fail() end
    if self.session then self.session:load_html(snap.html, snap.url)
    else self.session, self.browser = bridge.load_snapshot(snap) end
    self.revision, self.snapshots = snap.revision, self.snapshots + 1
    self.current = true
    _G.session, _G.document = self.session, self.session:document()
    if bridge.challenge(snap) then
        print('Anti-bot challenge inferred; finish human verification in Qutebrowser, then :capture again.')
    end
    self.stage = 'login beacon validation'
    self.login_evidence = bridge.login_evidence(self.session, self.options)
    self.confirmed = not self.options.require_login or self.login_evidence
    print(string.format('capture revision=%d bytes=%d login-evidence=%s', self.revision, #snap.html,
        self.login_evidence and 'true' or 'false'))
    if self.confirmed then self:discover()
    else print('Inspect with :targets / :links / :forms. Open an authenticated section or set qute:beacon(css, xpath).') end
end

function Console:beacon(css, xpath)
    self.options.success_selector, self.options.success_xpath = css, xpath
    if self.session and self.current then
        self.stage = 'login beacon validation'
        self.login_evidence = bridge.login_evidence(self.session, self.options)
        self.confirmed = not self.options.require_login or self.login_evidence
    end
    return self.login_evidence
end

function Console:ready()
    if not self.session or not self.current then fail() end
    return self.session:document()
end

function Console:discover()
    self.stage = 'endpoint discovery'
    self:ready()
    if not self.confirmed then fail() end
    local found = bridge.scrape(self.session, {include_noise=self.options.include_noise})
    local candidates, fresh = {}, 0
    for _, ep in ipairs(found.endpoints) do
        local ok, origin = pcall(prowse.url.origin, ep.url)
        if ok and origin == self.origin then
            if #candidates >= 10000 then fail() end
            candidates[#candidates + 1] = ep
        end
    end
    self.stage = 'schema enrichment'
    -- Enrich while each page's forms still exist. Keep structured, sanitized
    -- schemas, not entire historical HTML pages or their private form values.
    local enriched = bridge.enrich(self.session, candidates)
    for _, schema in ipairs(enriched.schemas) do
        local ep = schema.endpoint
        local key = tostring(ep.method):lower() .. ' ' .. schema.path_template
        if not self.seen[key] then
            if #self.endpoints >= 10000 then fail() end
            self.endpoints[#self.endpoints + 1], self.schemas[#self.schemas + 1] = ep, schema
            self.seen[key] = #self.schemas
            fresh = fresh + 1
        else
            local index = self.seen[key]
            local merged = bridge.plugin('schema_grabber').serialize({self.schemas[index], schema}, {
                redact_secrets=true, include_examples=false, include_provenance=true}).schemas[1]
            self.schemas[index], self.endpoints[index] = merged, merged.endpoint
        end
    end
    print(string.format('discovery new=%d total=%d (heuristic; response probes disabled)', fresh, #self.endpoints))
    if #self.endpoints == 0 then
        print('No useful API endpoints in this capture. Use :links or :targets, visit another application section, then :capture.')
        print('Inline forms/scripts are inspected; external bundles and historical network traffic are not in a DOM capture.')
    end
    return #self.endpoints
end

function Console:status()
    self.stage = 'bridge status'
    local status = self.client:status()
    return {revision=self.revision, snapshots=self.snapshots, endpoints=#self.endpoints,
        login_evidence=self.login_evidence == true, current_snapshot=self.current == true,
        connected=status.connected, closed=status.closed,
        actions_used=status.actions_used, max_actions=status.max_actions, transport=self.client.config.bulk or 'inline'}
end

function Console:selector(target)
    if type(target) == 'number' then
        if target % 1 ~= 0 or not self.indexed[target] then fail() end
        return self.indexed[target]
    end
    if type(target) ~= 'string' or #target == 0 or #target > 4096 then fail() end
    return target
end

function Console:act(order)
    self.stage = 'browser action'
    if self.client:status().connected ~= true then fail() end
    -- A lost action reply or FIFO body cannot prove the old snapshot is current.
    self.indexed, self.current = {}, false
    self.login_evidence, self.confirmed = false, false
    local snap = self.client:act(order, self.options.wait_ms or 30000)
    self:install(snap)
    self.stage = 'browser action'
    if snap.action_status == 'error' then fail() end
    if snap.action_status == 'unconfirmed' then print('Action marker absent; inspect the fresh DOM to confirm the result.') end
    return true
end
function Console:click(target) return self:act{type='click', selector=self:selector(target)} end
function Console:focus(target) return self:act{type='focus', selector=self:selector(target)} end
function Console:scroll(target) return self:act{type='scroll', selector=self:selector(target)} end
function Console:submit(target) return self:act{type='submit', selector=self:selector(target)} end
function Console:fill(target, value) return self:act{type='fill', selector=self:selector(target), value=value} end
function Console:select(target, value) return self:act{type='select', selector=self:selector(target), value=value} end
function Console:check(target, checked) return self:act{type='check', selector=self:selector(target), checked=checked} end
function Console:reload() return self:act{type='reload'} end
function Console:navigate(url)
    local absolute = prowse.url.resolve(self.session and self.session:current_url() or self.origin .. '/', url)
    return self:act{type='navigate', url=absolute}
end
function Console:capture()
    self.stage = 'snapshot receipt'
    if self.client:status().connected then return self:act{type='capture'} end
    local snap = self.client:snapshot(self.revision, self.options.wait_ms or 30000)
    if not snap then fail() end
    self:install(snap)
    return true
end

local function path_for(node)
    local parts = {}
    while node and node:tag_name() ~= '' and node:tag_name():sub(1, 1) ~= '#' do
        local tag = node:tag_name():lower()
        local parent, at = node:parent(), 1
        local sibling = node:previous_sibling()
        while sibling do
            if sibling:tag_name():lower() == tag then at = at + 1 end
            sibling = sibling:previous_sibling()
        end
        table.insert(parts, 1, tag .. ':nth-of-type(' .. math.max(at, 1) .. ')')
        node = parent
        if #parts > 256 then fail() end
    end
    return table.concat(parts, ' > ')
end

function Console:targets(selector)
    self.stage = 'DOM inspection'
    local nodes = self:ready():query_selector_all(selector or 'a, button, input, select, textarea, form, [role="button"]')
    local rows = {}
    self.indexed = {}
    for i, node in ipairs(nodes) do
        if i > 200 then break end
        local tag = node:tag_name():lower()
        self.indexed[i] = path_for(node)
        local row = {id=i, tag=tag, revision=self.revision, selector=self.indexed[i]}
        if tag == 'a' or tag == 'form' then
            local value = node:attribute(tag == 'a' and 'href' or 'action')
            local ok, absolute = pcall(prowse.url.resolve, self.session:current_url(), value)
            if ok and pcall(prowse.url.origin, absolute) then row.path = bare(absolute)
            else row.path = '[non-HTTP target]' end
        end
        if self.options.show_values then row.label = safe(node:attribute('aria-label') ~= '' and
            node:attribute('aria-label') or node:text()) end
        rows[#rows + 1] = row
    end
    if #nodes > #rows then print('Target list limited to 200; pass a narrower CSS selector to qute:targets(selector).') end
    return rows
end
function Console:links() return self:targets('a[href]') end
function Console:forms() return self:targets('form, input, select, textarea, button') end
function Console:show_values(enabled) self.options.show_values = enabled == true end
function Console:list_endpoints()
    local rows = {}
    for i, ep in ipairs(self.endpoints) do
        if i > 200 then break end
        rows[#rows + 1] = {method=ep.method, path=bare(self.schemas[i].path_template), confidence=ep.confidence,
            discovery_method=ep.discovery_method}
    end
    return rows
end

function Console:export(output, postman)
    self.stage = 'export validation'
    self:ready()
    if not self.confirmed or #self.schemas == 0 then fail() end
    self.stage = 'schema serialization'
    local rendered = bridge.plugin('schema_grabber').serialize(self.schemas, {
        redact_secrets=true, include_examples=false, include_provenance=true,
        collection_name='Discovered API (Qutebrowser interactive)'})
    local metadata = {snapshots=self.snapshots, actions_used=self.client:status().actions_used,
        login_evidence=self.login_evidence, authentication_verified=false, complete=false,
        response_probes=false, noise_filtered=self.options.include_noise ~= true}
    local yaml = rendered.openapi_yaml .. '\nx-prowsetk-qutebrowser:\n  interactive: true\n' ..
        '  snapshots: ' .. self.snapshots .. '\n  login-evidence: ' .. tostring(self.login_evidence) ..
        '\n  authentication-verified: false\n  complete: false\n  response-probes: false\n'
    local collection = bridge.json.decode(rendered.postman_json)
    collection['x-prowsetk-qutebrowser'] = metadata
    self.stage = 'OpenAPI export'
    bridge.write(output or self.options.output or '_scraped/booking-admin-openapi.yaml', yaml, self.options.ipc_module)
    self.stage = 'Postman export'
    bridge.write(postman or self.options.postman or '_scraped/booking-admin-postman.json', bridge.json.encode(collection), self.options.ipc_module)
    print(string.format('specs exported: %d operations from %d captures; coverage incomplete', rendered.schema_count, self.snapshots))
    return true
end

function Console:close()
    if self.session then self.session:close(); self.session = nil end
    _G.session, _G.document = nil, nil
    self.browser = nil
    if self.client then pcall(function() self.client:finish() end); self.client = nil end
end

local help = [[
:capture                  Ask the attached marionette for a fresh DOM capture
:status                   Connection, capture and discovery counters
:targets / :links / :forms Numbered structural targets (invalidated on capture)
:click TARGET             Click a target number or a literal CSS selector
:focus TARGET / :scroll TARGET / :submit TARGET
:fill NUMBER "value"       Set a normal text control using the native setter
:select NUMBER "value"     Select an option; :check NUMBER true|false
:navigate /relative/path  Same-origin navigation; :reload reloads the tab
:discover / :endpoints     Inspect forms/scripts and list accumulated API paths
:export                   Write accumulated OpenAPI + Postman specifications
:values on|off            Opt into local display of target labels (off by default)
:help / :quit             Help or close the interactive conversation

Lua expressions/statements persist. Examples:
  qute:click("button[data-next]")
  qute:fill("input[name='date']", "2027-01-01")
  qute:beacon("[data-testid='account-menu']")
  document:query_selector_all("form")
  require('lpdql').rows(session, 'select tag, text from <h*>')
  for _, ep in ipairs(qute.endpoints) do print(ep.method, ep.path) end
Multiline Lua uses a continuation prompt. Explicit Lua print/expressions can
display private page data; commands/errors never automatically print raw values.
Use application navigation, tabs, filters, pagination and lazy-load scrolling
to collect more captures. DOM snapshots supply no historical HAR or responses.
]]

function __qute_start(options)
    qute = setmetatable({}, Console)
    qute:start(options)
end
function __qute_close() if qute then qute:close() end end

local commands = {
    capture=function() return qute:capture() end, status=function() return qute:status() end,
    targets=function() return qute:targets() end, links=function() return qute:links() end,
    forms=function() return qute:forms() end, discover=function() return qute:discover() end,
    endpoints=function() return qute:list_endpoints() end, export=function() return qute:export() end,
    reload=function() return qute:reload() end, navigate=function(arg) return qute:navigate(arg) end,
    values=function(arg) if arg ~= 'on' and arg ~= 'off' then fail() end; qute:show_values(arg == 'on') end,
    help=function() print(help) end
}
for _, kind in ipairs({'click', 'focus', 'scroll', 'submit'}) do
    commands[kind] = function(arg) return qute[kind](qute, tonumber(arg) or arg) end
end
for _, kind in ipairs({'fill', 'select', 'check'}) do
    commands[kind] = function(arg)
        local id, value = arg:match('^(%d+)%s+(.+)$')
        if not id then fail() end
        local expression = load('return ' .. value, '=console value', 't', _G)
        if not expression then fail() end
        return qute[kind](qute, tonumber(id), expression())
    end
end

function __qute_eval(args)
    local line = args.source
    if #line > 262144 then return 'failed' end
    if line:match('^%s*:quit%s*$') then return 'quit' end
    qute.stage = 'Lua evaluation'
    local chunk
    local command, argument = line:match('^%s*:([%a_]+)%s*(.-)%s*$')
    if command then chunk = function() if not commands[command] then fail() end; return commands[command](argument) end
    else
        chunk = load('return ' .. line, '=console', 't', _G)
        if not chunk then
            local message
            chunk, message = load(line, '=console', 't', _G)
            if not chunk then
                if message:find('<eof>', 1, true) then return 'more' end
                io.stderr:write('qute: Lua syntax failed; check the statement (source omitted)\n')
                return 'failed'
            end
        end
    end
    local ok = pcall(function()
        local results = table.pack(chunk())
        for i = 1, results.n do display(results[i]) end
    end)
    if not ok then
        local hints = {
            ['browser action']='attach ptk-qute-marionette; use a current target; inspect the fresh DOM after failure',
            ['snapshot receipt']='attach the userscript or send a capture; the wait is bounded',
            ['snapshot parsing']='check the 16 MiB, 250000-node and 256-level bounds',
            ['export validation']='confirm login evidence and collect useful endpoints before exporting'
        }
        io.stderr:write('qute: ' .. qute.stage .. ' failed: ' .. (hints[qute.stage] or 'check :help; raw error omitted') .. '\n')
        return 'failed'
    end
    return 'ok'
end
