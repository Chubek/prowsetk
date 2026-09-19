-- Real DOM + endpoint extractor with an in-memory HTTP boundary.
local prowse = require('lprowse')
local browser = prowse.browser.new({javascript=false})
local dom_session = browser:create_session()
local calls = {}
local closed = false
local login = [[<form method="post" action="/auth"><input name="csrf" type="hidden" value="fixture-csrf">
<input type="email" name="email"><input type="password" name="password"></form>]]
if scenario == 'two-step' then
    login = [[<form method="post" action="/identify"><input type="email" name="email"></form>]]
elseif scenario == 'foreign-action' then
    login = [[<form method="post" action="https://untrusted.example/auth"><input name="email"><input type="password" name="password"></form>]]
elseif scenario == 'get-form' then
    login = login:gsub('method="post"', 'method="get"')
elseif scenario == 'challenge' then
    login = '<html><title>Verification required</title><div id="captcha"></div><a href="/sign-in">Sign in</a><script>fetch(\'/api/status\')</script></html>'
end
local wrapper = {}
function wrapper:load_html(...) return dom_session:load_html(...) end
function wrapper:document() return dom_session:document() end
function wrapper:current_url() return dom_session:current_url() end
function wrapper:close() closed = true; dom_session:close() end
function wrapper:request(method, url, options)
    calls[#calls+1] = {method=method, url=url, body=options.body}
    if url == 'https://booking.example/' then
        return {status=200,body=login,headers={}}
    elseif url == 'https://booking.example/identify' then
        assert(options.body == 'email=fixture%40example.com')
        return {status=200,body=[[<form method="post" action="/auth"><input type="password" name="password"></form>]],headers={}}
    elseif url == 'https://booking.example/auth' then
        assert(method == 'POST')
        assert(options.body:find('password=fixture%23pass%26word', 1, true))
        if scenario ~= 'two-step' then
            assert(options.body:find('csrf=fixture%-csrf'))
            assert(options.body:find('email=fixture%40example.com', 1, true))
        end
        if scenario == 'rejected' then return {status=200,body=login,headers={}} end
        if scenario == 'foreign-redirect' then
            return {status=307,body='',headers={Location='https://untrusted.example/auth'}}
        end
        return {status=303,body='',headers={Location='/dashboard'}}
    elseif url == 'https://booking.example/dashboard' then
        assert(method == 'GET' and options.body == '')
        return {status=200,body=[[<a href="/logout">Log out</a><a href="/reservations">Reservations</a>
<a href="/api/hotels?token=fixture-token">Hotels</a><script src="/app.js"></script>
<script>fetch('/api/inline')</script>]],headers={}}
    elseif url == 'https://booking.example/app.js' then
        return {status=200,body=[[fetch('/api/external')]],headers={}}
    end
    error('unexpected request')
end
prowse.browser.new = function(config)
    assert(config.follow_redirects == false and config.javascript == false)
    return {create_session=function() return wrapper end}
end
local original_getenv = os.getenv
os.getenv = function(key)
    if key:match('^BOOKING_') then return nil end
    if key == 'HOME' then return test_directory end
    return original_getenv(key)
end
local f = assert(io.open(dotenv_file,'w'))
if scenario == 'malformed-dotenv' then
    f:write('BOOKING_DOTCOM_URL="unclosed\n')
else
    f:write("export BOOKING_DOTCOM_URL='https://booking.example/' # test\n",
            'BOOKING_DOTCOM_USER="fixture@example.com"\n',
            "BOOKING_DOTCOM_PASS='fixture#pass&word'\n")
end
f:close()
dofile(driver_file)
local succeeded, message = pcall(main, {dotenv=dotenv_file, output=output_file})
if scenario == 'success' or scenario == 'two-step' then
    assert(succeeded, message)
    assert(message == 0)
    f = assert(io.open(output_file, 'r'))
    local yaml = f:read('*a'); f:close()
    for _, path in ipairs({'/reservations','/api/hotels','/api/inline','/api/external'}) do
        assert(yaml:find(path, 1, true), 'missing endpoint ' .. path)
    end
    assert(yaml:find('authenticated: true', 1, true))
    assert(yaml:find('complete: false', 1, true))
    for _, secret in ipairs({'fixture-token','fixture#pass&word','fixture@example.com','fixture-csrf'}) do
        assert(not yaml:find(secret, 1, true))
    end
    assert(#calls == (scenario == 'two-step' and 5 or 4))
elseif scenario == 'challenge' then
    -- No server-rendered login form (JS SPA); extraction proceeds unauthenticated.
    assert(succeeded, message)
    assert(message == 0)
    f = assert(io.open(output_file, 'r'))
    local yaml = f:read('*a'); f:close()
    assert(yaml:find('authenticated: false', 1, true))
    assert(yaml:find('complete: false', 1, true))
    assert(yaml:find('/sign-in', 1, true), 'missing unauthenticated endpoint /sign-in')
    assert(yaml:find('/api/status', 1, true), 'missing unauthenticated endpoint /api/status')
    -- Credentials were never sent.
    for _, secret in ipairs({'fixture#pass&word','fixture@example.com','fixture-csrf'}) do
        assert(not yaml:find(secret, 1, true), 'secret leaked in output: ' .. secret)
    end
    assert(#calls == 1, 'unexpected HTTP calls')
else
    assert(not succeeded, 'expected a failure')
    assert(not io.open(output_file, 'r'), 'failure produced output')
    assert(not message:find('fixture', 1, true), 'error leaked a secret')
    if scenario == 'get-form' or scenario == 'foreign-action' then
        assert(#calls == 1, 'credentials sent unexpectedly')
    elseif scenario == 'malformed-dotenv' then assert(#calls == 0)
    else assert(#calls == 2, 'password was retried or foreign origin requested') end
end
if scenario ~= 'malformed-dotenv' then assert(closed) end
