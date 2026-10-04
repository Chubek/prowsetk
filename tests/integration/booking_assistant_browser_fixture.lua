-- Real DOM/cookie import with a small, entirely in-memory HTTP boundary.
local prowse = require('lprowse')
local browser = prowse.browser.new({javascript=false})
local dom_session = browser:create_session()
local imported = 0
local closed = false
local requests = 0
local wrapper = {}
function wrapper:load_html(...) return dom_session:load_html(...) end
function wrapper:document() return dom_session:document() end
function wrapper:current_url() return dom_session:current_url() end
function wrapper:close() closed = true; dom_session:close() end
function wrapper:import_cookies_json(path)
    imported = dom_session:import_cookies_json(path)
    return imported
end
function wrapper:request(method, url)
    assert(imported == 1, 'HTTP confirmation preceded fresh cookie import')
    assert(method == 'GET' and url:find('https://admin.booking.com/', 1, true) == 1)
    requests = requests + 1
    assert(requests <= 8, 'handoff fixture exceeded its request budget')
    if url == 'https://admin.booking.com/' then
        return {status=200, headers={}, body='<h1>Joe Litty Rooms</h1>' ..
            '<a href="/logout">Log out</a><a href="/api/rooms">Rooms</a>'}
    elseif url == 'https://admin.booking.com/api/rooms' then
        return {status=200, headers={['Content-Type']='application/json'}, body='{"rooms":[]}'}
    end
    return {status=404, headers={}, body=''}
end
prowse.browser.new = function(config)
    assert(config.follow_redirects == false and config.javascript == true)
    return {create_session=function() return wrapper end}
end

dofile(driver_file)
assert(main({
    assistant_browser_force=true,
    success_beacon="xpath=//h1[contains(normalize-space(.), 'Joe Litty Rooms')]",
    success_beacon_type='xpath',
    cookies_json=cookies_file,
    dotenv=dotenv_file,
    output=output_file,
    postman=postman_file,
    max_depth=0,
    max_pages=1,
    max_api_requests=1,
    max_resolve_rounds=1,
    max_schema_probes=0
}) == 0)
assert(imported == 1 and closed and requests > 0)
