-- lopencode: pure-Lua convenience wrapper around the native module.
-- The native `lopencode` module (luaopen_lopencode) provides `client.new`.
-- This file documents driver usage and adds a one-call scrape helper.
local native = require("lopencode")

local M = {}
M.client = native.client
M.new = native.new
M._version = native._version

--- Scrape the current document of a prowse session with an agent prompt.
---@param client table OpenCode client from client.new
---@param session table lprowse session or document (or snapshot table)
---@param instructions string extraction goals
---@param schema string|nil expected JSON object schema (validated as object)
function M.scrape(client, session, instructions, schema)
    return client:scrape_with_prompt(session, instructions, schema)
end

--- Build the subtractive endpoint-cleanup prompt over a redacted JSON array
--- of endpoint objects. Pure function: no client, no transport.
---@param endpoints_json string redacted JSON array of {url, method, ...}
---@param instructions string|nil extra cleanup instructions
function M.build_cleanup_prompt(endpoints_json, instructions)
    return native.build_cleanup_prompt(endpoints_json, instructions)
end

return M
