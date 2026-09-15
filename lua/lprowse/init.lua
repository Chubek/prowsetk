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

-- `prowse.browser.new([config])` creates a Browser.
lprowse.browser = {
    new = function(config)
        return require("lprowse").browser.new(config)
    end
}

return lprowse