-- lprowseir.lua
-- Intermediate-representation helpers for ProwseTk.

local lprowseir = {
    _version = "0.1.0",
    _description = "ProwseTk IR emitters and XPath listeners"
}

local function native_unavailable(feature)
    error(feature .. " requires ProwseTk's native lprowseir module", 3)
end

-- Emits the four built-in IR formats from a document/session.
function lprowseir.emit_dom(document_or_session)
    native_unavailable("lprowseir.emit_dom")
end

function lprowseir.emit_xas(document_or_session)
    native_unavailable("lprowseir.emit_xas")
end

function lprowseir.emit_vtd(document_or_session)
    native_unavailable("lprowseir.emit_vtd")
end

function lprowseir.emit_iml(document_or_session)
    native_unavailable("lprowseir.emit_iml")
end

lprowseir.dom = {
    walk = function(document_or_session, xpath, callback)
        native_unavailable("lprowseir.dom.walk")
    end
}

lprowseir.xas = {
    AddListener = function(document_or_session, xpath, callback)
        native_unavailable("lprowseir.xas.AddListener")
    end,
    add_listener = function(document_or_session, xpath, callback)
        native_unavailable("lprowseir.xas.add_listener")
    end
}

-- Backward-compatible alias used in AGENTS.md examples.
lprowseir.xax = lprowseir.xas

package.loaded["lprowseir.dom"] = lprowseir.dom
package.loaded["lprowseir.xas"] = lprowseir.xas
package.loaded["lprowseir.xax"] = lprowseir.xax

return lprowseir
