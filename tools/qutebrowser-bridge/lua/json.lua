-- Strict JSON for bridge data; never evaluates Lua or page-supplied code.
local json = { null = {} }
local array_kind = { __json_array = true }
local function fail() error('invalid bridge JSON', 0) end
local escapes = { ['"']='"', ['\\']='\\', ['/']='/', b='\b', f='\f', n='\n', r='\r', t='\t' }

function json.decode(source)
    if type(source) ~= 'string' or #source > 8 * 1024 * 1024 then fail() end
    local at, parse = 1
    local function whitespace()
        local _, last = source:find('^[ \t\r\n]*', at)
        at = (last or at - 1) + 1
    end
    local function text()
        if source:sub(at, at) ~= '"' then fail() end
        at = at + 1
        local parts = {}
        while true do
            local pos = source:find('["\\%c]', at)
            if not pos then fail() end
            parts[#parts + 1] = source:sub(at, pos - 1)
            local char = source:sub(pos, pos)
            at = pos + 1
            if char == '"' then
                local result = table.concat(parts)
                if not utf8.len(result) then fail() end
                return result
            end
            if char ~= '\\' then fail() end
            local escaped = source:sub(at, at)
            at = at + 1
            if escaped == 'u' then
                local hex = source:sub(at, at + 3)
                if #hex ~= 4 or not hex:match('^%x+$') then fail() end
                local code = tonumber(hex, 16)
                at = at + 4
                if code >= 0xd800 and code <= 0xdbff then
                    if source:sub(at, at + 1) ~= '\\u' then fail() end
                    hex = source:sub(at + 2, at + 5)
                    if #hex ~= 4 or not hex:match('^%x+$') then fail() end
                    local low = tonumber(hex, 16)
                    if low < 0xdc00 or low > 0xdfff then fail() end
                    code, at = 0x10000 + (code - 0xd800) * 1024 + low - 0xdc00, at + 6
                elseif code >= 0xdc00 and code <= 0xdfff then fail() end
                parts[#parts + 1] = utf8.char(code)
            elseif escapes[escaped] then parts[#parts + 1] = escapes[escaped]
            else fail() end
        end
    end
    parse = function(depth)
        if depth > 32 then fail() end
        whitespace()
        local char = source:sub(at, at)
        if char == '"' then return text() end
        if char == '[' or char == '{' then
            local object, result, seen = char == '{', {}, {}
            if not object then setmetatable(result, array_kind) end
            local closing = object and '}' or ']'
            at = at + 1
            whitespace()
            if source:sub(at, at) == closing then at = at + 1; return result end
            while true do
                whitespace()
                local key = #result + 1
                if object then
                    key = text()
                    if seen[key] then fail() end
                    seen[key] = true
                    whitespace()
                    if source:sub(at, at) ~= ':' then fail() end
                    at = at + 1
                end
                result[key] = parse(depth + 1)
                whitespace()
                char = source:sub(at, at)
                at = at + 1
                if char == closing then return result end
                if char ~= ',' then fail() end
            end
        end
        for literal, value in pairs({ ['true']=true, ['false']=false, ['null']=json.null }) do
            if source:sub(at, at + #literal - 1) == literal then at = at + #literal; return value end
        end
        local start = at
        if char == '-' then at = at + 1 end
        char = source:sub(at, at)
        if char == '0' then at = at + 1
        elseif char:match('^[1-9]$') then
            local _, last = source:find('^[0-9]+', at); at = last + 1
        else fail() end
        if source:sub(at, at) == '.' then
            at = at + 1
            local _, last = source:find('^[0-9]+', at)
            if not last then fail() end
            at = last + 1
        end
        if source:sub(at, at):match('^[eE]$') then
            at = at + 1
            if source:sub(at, at):match('^[+-]$') then at = at + 1 end
            local _, last = source:find('^[0-9]+', at)
            if not last then fail() end
            at = last + 1
        end
        local value = tonumber(source:sub(start, at - 1))
        if not value or value ~= value or value == math.huge or value == -math.huge then fail() end
        return value
    end
    local result = parse(0)
    whitespace()
    if at <= #source then fail() end
    return result
end

function json.encode(value)
    local seen = {}
    local function text(s)
        if not utf8.len(s) then fail() end
        return '"' .. s:gsub('[%z\1-\31\\"]', function(c)
            if c == '"' or c == '\\' then return '\\' .. c end
            return string.format('\\u%04x', c:byte())
        end) .. '"'
    end
    local function emit(v, depth)
        if depth > 32 then fail() end
        if v == json.null or v == nil then return 'null' end
        if type(v) == 'string' then return text(v) end
        if type(v) == 'boolean' then return v and 'true' or 'false' end
        if type(v) == 'number' then
            if v ~= v or v == math.huge or v == -math.huge then fail() end
            return string.format('%.17g', v)
        end
        if type(v) ~= 'table' or seen[v] then fail() end
        seen[v] = true
        local array, count, names = getmetatable(v) == array_kind or #v > 0, 0, {}
        for key in pairs(v) do
            count = count + 1
            if type(key) ~= 'number' or key % 1 ~= 0 or key < 1 or key > #v then array = false end
            names[#names + 1] = key
        end
        local parts = {}
        if array and count == #v then
            for i = 1, #v do parts[#parts + 1] = emit(v[i], depth + 1) end
        else
            for _, key in ipairs(names) do if type(key) ~= 'string' then fail() end end
            table.sort(names)
            for _, key in ipairs(names) do parts[#parts + 1] = text(key) .. ':' .. emit(v[key], depth + 1) end
        end
        seen[v] = nil
        return (array and '[' or '{') .. table.concat(parts, ',') .. (array and ']' or '}')
    end
    local result = emit(value, 0)
    if #result > 8 * 1024 * 1024 then fail() end
    return result
end
return json
