-- Adapter for `prowsetk run` as well as the standalone crawler host.
local source = debug.getinfo(1, 'S').source
if source:sub(1,1) == '@' then source = source:sub(2) end
local directory = source:match('^(.*[/\\])') or './'
local function load(name, path, installed)
    if not package.loaded[name] then
        local file = io.open(directory .. path, 'rb')
        if file then file:close() else path = installed or path end
        package.loaded[name] = dofile(directory .. path)
    end
end
load('ezlogin', '../../plugins/ezlogin/lua/ezlogin.lua', '../plugins/ezlogin/ezlogin.lua')
load('scrape_endpoints', '../../plugins/scrape-endpoints/lua/scrape_endpoints.lua', '../plugins/scrape-endpoints/scrape_endpoints.lua')
load('lcrawler', 'lua/lcrawler.lua')
function main(args)
    local browser = require('lprowse').browser.new()
    local active = session or browser:create_session()
    local code = require('lcrawler').run(active, args)
    if code ~= 0 then return code end
    if args.output and args.output ~= '' then
        local file = assert(io.open(args.output, 'wb'))
        file:write(__crawler_result{kind='pages'}); file:close()
    end
    return 0
end
