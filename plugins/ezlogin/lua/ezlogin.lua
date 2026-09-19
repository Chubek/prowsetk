-- Lua helper for drivers. Credentials are applied to the session only and are
-- never returned or printed. Network access remains host-mediated by Session.
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
    else
        error("ezlogin: unsupported mode " .. tostring(mode), 2)
    end
    return true
end

return M
