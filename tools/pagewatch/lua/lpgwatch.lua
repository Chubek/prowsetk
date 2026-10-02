-- Managed DOM watchers. The daemon host refreshes the session and transports
-- serialized changes over IPC; this library never opens a network socket.
local pdql = require('lpdql')
local M = {}
local current
local Watcher = {}
Watcher.__index = Watcher
function M.watch(options)
    assert(type(options) == 'table', 'lpgwatch.watch requires an options table')
    assert(options.session, 'lpgwatch.watch requires a managed session')
    pdql.validate(assert(options.query, 'lpgwatch.watch requires a PDQL query'))
    assert(not current, 'one watcher is supported per deployed script')
    current = setmetatable({session=options.session, query=options.query,
        before_sample=options.before_sample, previous=nil}, Watcher)
    return current
end
function Watcher:sample()
    if self.before_sample then self.before_sample(self.session) end
    local data = pdql.query(self.session, self.query, 'json')
    local changed = self.previous ~= data
    self.previous = data
    return data, changed
end
function __pgwatch_sample()
    assert(current, 'driver must register a watcher in main(args)')
    return current:sample()
end
return M
