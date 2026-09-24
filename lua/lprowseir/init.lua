-- lprowseir.lua
-- Intermediate-representation helpers for ProwseTk.
--
-- The native module (registered by LuaRuntime) implements four built-in IRs
-- over the Flatworm DOM, all with pugixml-powered XPath selection:
--
-- - ProwseXAS: an event stream (`emit_xas`, `xas:AddListener`).
-- - ProwseDOM: a flattened, walkable node model (`emit_dom`, `dom.walk`).
-- - ProwseVTD: a binary virtual-token dump (`emit_vtd`, `emit(doc, "vtd")`).
-- - ProwseIML: an S-expression form with macros (`emit_iml`,
--   `emit(doc, "iml")`).
--
-- Native plugins may register further named IRs with IrEmitterRegistry;
-- drivers resolve every name uniformly through `emit(document, name)` and
-- list them with `emitters()`.

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

-- Emits a named IR through the plugin-extensible registry ("vtd", "iml",
-- plus any format a native plugin registered). Binary IRs come back as byte
-- strings. Unknown names raise.
function lprowseir.emit(document_or_session, name)
    native_unavailable("lprowseir.emit")
end

-- Lists every registered IR name, built-ins included.
function lprowseir.emitters()
    native_unavailable("lprowseir.emitters")
end

lprowseir.dom = {
    -- Walks the elements matching `xpath`, invoking `callback(element)`
    -- per match. Returns the number of invocations. An invalid XPath
    -- raises; a failing callback aborts the walk and re-raises.
    walk = function(document_or_session, xpath, callback)
        native_unavailable("lprowseir.dom.walk")
    end
}

lprowseir.xas = {
    -- Invokes `callback(event)` for each ProwseXAS event in the subtrees
    -- matching `xpath`. Returns the number of invocations. An invalid
    -- XPath raises; a failing callback aborts and re-raises.
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
