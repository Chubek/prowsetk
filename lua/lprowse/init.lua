-- lprowse.lua
-- The primary programmable browser API for ProwseTk.
--
-- The C++ runtime registers `lprowse` natively; this file mirrors that module
-- for standalone embedding and documents the surface. Prefer the native module
-- provided by the host (`require("lprowse")`).

local lprowse = {
    _version = "0.1.0",
    _description = "ProwseTk browser automation API"
}

local function native_unavailable()
    error("lprowse native module is not registered; run inside ProwseTk's LuaRuntime", 2)
end

-- `prowse.browser.new([config])` creates a Browser.
lprowse.browser = {
    new = native_unavailable
}

return lprowse
