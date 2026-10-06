-- Shared positive-DOM login beacon for the driver and interactive console.
return function(active, args)
    args = args or {}
    local doc = active:document()
    local function eligible(node)
        local ancestor = node
        while ancestor do
            local tag = ancestor:tag_name():lower()
            if tag == 'script' or tag == 'style' or tag == 'meta' or tag == 'template' then return false end
            ancestor = ancestor:parent()
        end
        return true
    end
    local function matched(nodes)
        for _, node in ipairs(nodes) do if eligible(node) then return true end end
        return false
    end
    if args.success_xpath and args.success_xpath ~= '' then return matched(doc:xpath(args.success_xpath)) end
    if args.success_selector and args.success_selector ~= '' then
        return matched(doc:query_selector_all(args.success_selector))
    end
    if matched(doc:query_selector_all("[data-testid='account-menu']")) then return true end
    local function label(value)
        local text = (value or ''):lower():gsub('[%s_%-]', '')
        return text == 'logout' or text == 'signout' or text == 'logoff' or text == 'signoff'
    end
    local function path(value)
        local bare = (value or ''):lower():gsub('[?#].*$', ''):gsub('^https?://[^/]+', '')
        for part in bare:gmatch('[^/]+') do if label(part:gsub('%.[%w]+$', '')) then return true end end
        return false
    end
    for _, node in ipairs(doc:query_selector_all('a, button, input, form')) do
        if eligible(node) then
            local tag = node:tag_name():lower()
            if (tag == 'a' and path(node:attribute('href'))) or (tag == 'form' and path(node:attribute('action'))) or
               ((tag == 'button' or tag == 'input') and path(node:attribute('formaction'))) then return true end
            if tag == 'a' or tag == 'button' or (tag == 'input' and
                (node:attribute('type'):lower() == 'submit' or node:attribute('type'):lower() == 'button')) then
                if label(node:attribute('aria-label')) or label(node:attribute('title')) or
                   (tag == 'input' and label(node:attribute('value'))) then return true end
                if not node:query_selector('script, style, template') and label(node:text()) then return true end
            end
        end
    end
    return false
end
