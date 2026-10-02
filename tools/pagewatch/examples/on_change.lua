-- Trusted action run in a separate, deadline-bounded process. Do not print
-- page data: write only the explicitly selected sanitized payload as needed.
function main(args)
    local file = assert(io.open('pagewatch-' .. args.name .. '.json', 'wb'))
    file:write(args.data)
    file:close()
    return 0
end
