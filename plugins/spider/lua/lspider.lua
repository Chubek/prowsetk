-- Injected by the spider host into each driver invocation. The live session is
-- shared; Lua handles remain managed by lprowse. Cache/frontier writes are staged
-- and committed only after main(args) succeeds. Browser actions are immediate.
local session = assert(session, "lspider requires a spider host")
local pending = {}
local records = __spider_records or {}
local library = {}
function library.session() return session end
function library.document() return session:document() end
function library.load_html(html, url) return session:load_html(html, url) end
function library.navigate(url) return session:navigate(url) end
local function element(selector, index)
    local nodes = session:document():query_selector_all(selector)
    return assert(nodes[index or 1], "element not found")
end
function library.click(selector, index) return element(selector, index):click() end
function library.type(selector, text, index) return element(selector, index):type(text) end
function library.enqueue(url, depth)
    assert(type(url) == "string" and #pending < 1000, "invalid frontier operation")
    pending[#pending + 1] = {"enqueue", url, tostring(depth or 0)}
end
library.cache = {}
function library.cache.get(key) return records[key] end
function library.cache.query(prefix)
    local result = {}
    for key, value in pairs(records) do
        if key:sub(1, #(prefix or "")) == (prefix or "") then result[key] = value end
    end
    return result
end
function library.cache.put(key, value)
    assert(type(key) == "string" and type(value) == "string" and #pending < 1000,
           "invalid cache operation")
    pending[#pending + 1] = {"put", key, value}
    records[key] = value
end
local function packed(values)
    local out = {}
    for _, value in ipairs(values) do
        assert(not value:find("\0", 1, true), "NUL in driver operation")
        out[#out + 1] = #value .. ":" .. value
    end
    return table.concat(out)
end
function __spider_finish()
    local out = {}
    for _, operation in ipairs(pending) do out[#out + 1] = packed(operation) end
    return packed(out)
end
package.loaded.lspider = library
return library
