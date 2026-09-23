-- Fixture for testing booking-dotcom driver with ezlogin form login and recursive crawl.
-- Real DOM + endpoint extractor with an in-memory HTTP boundary.
local prowse = require('lprowse')
local browser = prowse.browser.new({javascript=true})
local dom_session = browser:create_session()
local calls = {}
local closed = false
local cookie_session = scenario == 'cookie-session' or scenario == 'cookie-admin-path'
local authenticated = cookie_session

-- Login page HTML
local login = [[<form method="post" action="/auth"><input name="_csrf" type="hidden" value="fixture-csrf">
<input type="email" name="username"><input type="password" name="password"></form>]]
if scenario == 'two-step' then
    login = [[<form method="post" action="/identify"><input type="email" name="username"></form>]]
elseif scenario == 'js-built-form' then
    login = [[<html class="no-js"><head><title>Sign in</title></head><body>
    <div id="mount"></div><noscript>Please enable JavaScript in your browser to proceed</noscript>
    <script>
    document.getElementById('mount').innerHTML = '<form method="post" action="/auth">'
      + '<input name="_csrf" type="hidden" value="fixture-csrf">'
      + '<input type="email" name="username">'
      + '<input type="password" name="password">'
      + '</form>';
    document.documentElement.classList.remove('no-js');
    document.documentElement.classList.add('js');
    </script></body></html>]]
elseif scenario == 'foreign-action' then
    login = [[<form method="post" action="https://untrusted.example/auth"><input name="username"><input type="password" name="password"></form>]]
elseif scenario == 'get-form' then
    login = login:gsub('method="post"', 'method="get"')
elseif scenario == 'challenge' then
    login = '<html><title>Verification required</title><div id="captcha"></div><a href="/sign-in">Sign in</a><script>fetch(\'/api/status\')</script></html>'
elseif scenario == 'js-required' then
    login = '<html class="no-js"><noscript>Please enable JavaScript in your browser to proceed</noscript><script src="https://cf.bstatic.com/app.js"></script></html>'
end

-- Dashboard page (shown after successful login)
local dashboard = [[<a href="/logout">Log out</a><a href="/reservations">Reservations</a>
<a href="/api/hotels?token=fixture-token">Hotels</a><a href="/hotel/hoteladmin">Hotel admin</a>
<a href="/partner-settings/security">Partner security</a><script src="/app.js"></script>
<script>fetch('/api/inline')</script>]]
if scenario == 'js-dashboard' then
    dashboard = dashboard:gsub('<a href="/logout">Log out</a>',
        [[<div id="account"></div><script>document.getElementById('account').innerHTML = '<a href="/sign-out">Exit</a>';</script>]])
elseif scenario == 'script-marker' then
    dashboard = [[<script>var messages = {logout: 'Log out'};</script><p>Sign in required</p>]]
end

-- Reservations page (for recursive crawl)
local reservations = [[<a href="/logout">Log out</a><a href="/dashboard">Dashboard</a>
<script>fetch('/api/bookings'); fetch('/partner-settings/reservations')</script>]]

local wrapper = {}
function wrapper:load_html(...) return dom_session:load_html(...) end
function wrapper:document() return dom_session:document() end
function wrapper:current_url() return dom_session:current_url() end
function wrapper:close() closed = true; dom_session:close() end
function wrapper:set_header(name, value)
    if name == 'Cookie' and value and value:find('session=fixture%-session%-token') then
        authenticated = true
    end
end

function wrapper:request(method, url, options)
    options = options or {}
    calls[#calls+1] = {method=method, url=url, body=options.body}
    
    if url == 'https://admin.booking.com/' then
        if scenario == 'redirect-loop' then
            return {status=302, body='', headers={Location='/'}}
        elseif scenario == 'downgrade-redirect' then
            return {status=302, body='', headers={Location='http://admin.booking.com/auth'}}
        end
        if scenario == 'oauth' and not authenticated then
            return {status=302, body='', headers={Location='https://account.booking.com/sign-in?op_token=fixture-op-token'}}
        end
        if authenticated then
            if scenario == 'redirect-dashboard' or cookie_session then
                return {status=302, body='', headers={Location='/landing'}}
            end
            return {status=scenario == 'http-error-marker' and 401 or 200, body=dashboard, headers={}}
        end
        return {status=200, body=login, headers={}}
    elseif url == 'https://admin.booking.com/landing' then
        return {status=303, body='', headers={Location=scenario == 'cookie-admin-path' and
            '/hotel/hoteladmin/index.html' or '/dashboard'}}
    elseif url == 'https://admin.booking.com/hotel/hoteladmin/index.html' then
        return {status=200, body=dashboard, headers={}}
    elseif url == 'https://account.booking.com/sign-in?op_token=fixture-op-token' then
        return {status=200, body='<html><body><noscript>Please enable JavaScript</noscript></body></html>', headers={}}
    elseif url == 'https://account.booking.com/account/sign-in/login_name' then
        assert(method == 'POST')
        assert(options.body:find('"login_name":"fixture@example.com"', 1, true))
        assert(options.body:find('"op_token":"fixture-op-token"', 1, true))
        assert(not options.body:find('fixture#pass&word', 1, true),
               'password sent to login_name endpoint')
        return {status=200, body='{"state":"fixture-state","code_challenge":"fixture-code"}', headers={}}
    elseif url == 'https://account.booking.com/account/sign-in/password' then
        assert(method == 'POST')
        assert(options.body:find('"password":"fixture#pass&word"', 1, true))
        assert(options.body:find('"state":"fixture-state"', 1, true))
        return {status=200, body='{"redirect_uri":"https://admin.booking.com/dashboard"}',
                headers={['Set-Cookie']='session=fixture-session-token; Path=/; HttpOnly'}}
    elseif url == 'https://admin.booking.com/identify' then
        assert(options.body == 'username=fixture%40example.com')
        return {status=200, body=[[<form method="post" action="/auth"><input type="password" name="password"></form>]], headers={}}
    elseif url == 'https://admin.booking.com/auth' then
        assert(method == 'POST')
        assert(options.body:find('password=fixture%23pass%26word', 1, true))
        if scenario ~= 'two-step' then
            assert(options.body:find('_csrf=fixture%-csrf'))
            assert(options.body:find('username=fixture%40example.com', 1, true))
        end
        if scenario == 'rejected' then return {status=200, body=login, headers={}} end
        if scenario == 'foreign-redirect' then
            return {status=307, body='', headers={Location='https://untrusted.example/auth'}}
        end
        return {status=303, body='', headers={Location='/dashboard', ['Set-Cookie']='session=fixture-session-token; Path=/; HttpOnly'}}
    elseif url == 'https://admin.booking.com/dashboard' then
        assert(method == 'GET' and (options.body == nil or options.body == ''))
        -- The login helper can succeed; the driver must still verify the final page.
        return {status=200, body=(scenario == 'script-marker' or scenario == 'js-dashboard') and
            '<a href="/logout">Log out</a>' or dashboard, headers={}}
    elseif url == 'https://admin.booking.com/app.js' then
        return {status=200, body=[[fetch('/api/external'); import('/static/chunk.js'); fetch('/api/checkin',{method:'POST'}); $.post('/api/activity',{visit:1}); $.ajax({url:'/api/legacy',type:'POST'}); navigator.sendBeacon('/__challenge_fixture/telemetry', 't=1'); fetch('https://booking.com/__challenge_fixture/telemetry',{method:"POST"})]], headers={['Content-Type']='application/javascript'}}
    elseif url == 'https://admin.booking.com/static/chunk.js' then
        return {status=200, body=[[fetch('/gateway/rates')]], headers={['Content-Type']='application/javascript'}}
    elseif url == 'https://admin.booking.com/reservations' then
        -- For recursive crawl - only reached in success/two-step scenarios
        return {status=200, body=reservations, headers={}}
    elseif url == 'https://admin.booking.com/api/hotels?token=fixture-token' then
        return {status=200, body='{"hotels":[],"next":"/api/hotel-details"}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/api/inline' then
        return {status=200, body='{"inline":true}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/api/external' then
        return {status=200, body='{"external":true}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/api/bookings' then
        -- Endpoint referenced in reservations page script
        return {status=200, body='{"bookings":[],"next":"/api/bookings/details","related":["/api/availability"]}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/api/hotel-details' then
        return {status=200, body='{"details":[]}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/api/bookings/details' then
        return {status=200, body='{"details":[]}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/api/availability' then
        return {status=200, body='{"availability":[]}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/gateway/rates' then
        return {status=200, body='{"rates":[],"next":"/service/pricing"}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/service/pricing' then
        return {status=200, body='{"pricing":[]}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/hotel/hoteladmin' then
        return {status=200, body='{"hoteladmin":[],"next":"/partner-settings/policies"}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/partner-settings/security' then
        return {status=200, body='{"security":[],"next":"/partner-settings/users"}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/partner-settings/reservations' then
        return {status=200, body='{"reservations":[]}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/partner-settings/policies' then
        return {status=200, body='{"policies":[]}', headers={['Content-Type']='application/json'}}
    elseif url == 'https://admin.booking.com/partner-settings/users' then
        return {status=200, body='{"users":[]}', headers={['Content-Type']='application/json'}}
    end
    error('unexpected request: ' .. method .. ' ' .. url)
end

prowse.browser.new = function(config)
    assert(config.follow_redirects == false and config.javascript == true)
    return {create_session=function() return wrapper end}
end

local original_getenv = os.getenv
os.getenv = function(key)
    if key:match('^BOOKING_') then return nil end
    if key == 'HOME' then return test_directory end
    return original_getenv(key)
end

-- Write dotenv file
local f = assert(io.open(dotenv_file,'w'))
if cookie_session then
    f:write('# Existing session needs no username or password\n')
elseif scenario == 'malformed-dotenv' then
    f:write('BOOKING_DOTCOM_USER="unclosed\n')
else
    f:write('BOOKING_DOTCOM_USER="fixture@example.com"\n',
            "BOOKING_DOTCOM_PASS='fixture#pass&word'\n")
end
f:close()

-- Run the driver
dofile(driver_file)
local succeeded, message = pcall(main, {
    dotenv=dotenv_file,
    output=output_file,
    postman=postman_file,
    max_depth='1',  -- Crawl one level deep (dashboard -> reservations)
    max_pages='3'   -- Limit pages for test determinism
})

-- Verify results based on scenario
if scenario == 'success' or scenario == 'js-built-form' or scenario == 'two-step' or scenario == 'oauth' or
   scenario == 'redirect-dashboard' or scenario == 'js-dashboard' or cookie_session then
    assert(succeeded, message)
    assert(message == 0)
    
    -- Verify OpenAPI YAML
    f = assert(io.open(output_file, 'r'))
    local yaml = f:read('*a'); f:close()
    for _, path in ipairs({'/reservations','/api/hotels','/api/inline','/api/external',
                           '/api/bookings','/api/bookings/details','/api/availability',
                           '/api/hotel-details','/gateway/rates','/service/pricing',
                           '/hotel/hoteladmin','/partner-settings/security',
                           '/partner-settings/reservations','/partner-settings/policies',
                           '/partner-settings/users'}) do
        assert(yaml:find(path, 1, true), 'missing endpoint ' .. path)
    end
    -- POST endpoints discovered method-aware in the fetched /app.js bundle
    -- (fetch POST, $.post, $.ajax POST, sendBeacon, cross-origin telemetry).
    for _, path in ipairs({'/api/checkin','/api/activity','/api/legacy',
                           '/__challenge_fixture/telemetry'}) do
        assert(yaml:find(path, 1, true), 'missing POST endpoint ' .. path)
    end
    assert(yaml:find('post:', 1, true), 'missing post method in YAML')
    assert(yaml:find('has-post: true', 1, true), 'restful status missing has-post')
    assert(yaml:find('authenticated: true', 1, true))
    assert(yaml:find('complete: false', 1, true))
    for _, secret in ipairs({'fixture-token','fixture#pass&word','fixture@example.com','fixture-csrf'}) do
        assert(not yaml:find(secret, 1, true), 'secret leaked in YAML: ' .. secret)
    end
    
    -- Verify Postman JSON exists and contains endpoints
    f = assert(io.open(postman_file, 'r'))
    local postman = f:read('*a'); f:close()
    assert(postman:find('/api/hotels', 1, true), 'Postman missing /api/hotels')
    assert(postman:find('/api/bookings/details', 1, true), 'Postman missing recursive /api/bookings/details')
    assert(postman:find('/service/pricing', 1, true), 'Postman missing recursive /service/pricing')
    assert(postman:find('/hotel/hoteladmin', 1, true), 'Postman missing /hotel/hoteladmin')
    assert(postman:find('/partner-settings/users', 1, true), 'Postman missing recursive /partner-settings/users')
    assert(postman:find('Discovered API', 1, true), 'Postman missing info')
    for _, secret in ipairs({'fixture-token','fixture#pass&word','fixture@example.com'}) do
        assert(not postman:find(secret, 1, true), 'secret leaked in Postman: ' .. secret)
    end
    
    local minimum_calls = (scenario == 'two-step' or scenario == 'oauth') and 14 or 13
    assert(#calls >= minimum_calls, 'recursive API crawl made too few calls: ' .. #calls)
    if cookie_session then
        for _, call in ipairs(calls) do assert(call.method == 'GET', 'cookie session posted credentials') end
    end
    
elseif scenario == 'challenge' then
    assert(not succeeded, 'expected a challenge failure')
    assert(not io.open(output_file, 'r'), 'challenge produced output')
    assert(message:find('captcha-handler detected a confirmed challenge', 1, true), message)
    for _, secret in ipairs({'fixture#pass&word','fixture@example.com','fixture-csrf'}) do
        assert(not message:find(secret, 1, true), 'error leaked a secret: ' .. secret)
    end
    assert(#calls == 1, 'unexpected HTTP calls: ' .. #calls)
    
elseif scenario == 'js-required' then
    assert(not succeeded, 'expected a JavaScript-required failure')
    assert(not io.open(output_file, 'r'), 'JavaScript-required page produced output')
    assert(message:find('requires browser JavaScript or captcha support', 1, true), message)
    for _, secret in ipairs({'fixture#pass&word','fixture@example.com','fixture-csrf'}) do
        assert(not message:find(secret, 1, true), 'error leaked a secret: ' .. secret)
    end
    assert(#calls == 2, 'unexpected HTTP calls: ' .. #calls)
    
else
    -- Other failure scenarios
    assert(not succeeded, 'expected a failure')
    assert(not io.open(output_file, 'r'), 'failure produced output')
    assert(not message:find('fixture', 1, true), 'error leaked a secret')
    if scenario == 'redirect-loop' then
        assert(#calls == 9, 'redirect limit was not enforced')
        assert(message:find('exceeded 8 login redirects', 1, true), message)
    elseif scenario == 'downgrade-redirect' then
        assert(#calls == 1, 'insecure redirect was followed')
    elseif scenario == 'script-marker' or scenario == 'http-error-marker' then
        assert(message:find('login was not confirmed', 1, true), message)
    elseif scenario == 'get-form' or scenario == 'foreign-action' then
        for _, call in ipairs(calls) do
            assert(call.method ~= 'POST', 'credentials sent unexpectedly')
        end
        assert(#calls == 2, 'unexpected call count: ' .. #calls)
    elseif scenario == 'malformed-dotenv' then
        assert(#calls == 0, 'unexpected calls with malformed dotenv: ' .. #calls)
    else
        assert(#calls == 3, 'unexpected call count: ' .. #calls)
    end
end

if scenario ~= 'malformed-dotenv' then assert(closed) end
