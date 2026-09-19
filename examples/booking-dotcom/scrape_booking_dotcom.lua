local source = debug.getinfo(1, "S").source
if source:sub(1, 1) == "@" then source = source:sub(2) end
local here = source:match("^(.*)/[^/]*$") or "."
local target = here .. "/../booking-dotcom-admin-scrape/scrape-booking-dotcom-admin.lua"
dofile(target)
