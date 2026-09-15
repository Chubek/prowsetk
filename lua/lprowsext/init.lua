-- lprowsext.lua
-- Lua extension layer for ProwseTk.
--
-- This file documents the extension surface that the C++ runtime registers as
-- the `lprowsext` module. Applications use it to register document processors,
-- build extractors, run XPath DOM queries, extract endpoints, and manage WASM
-- plugin handles.

local lprowsext = {
    _version = "0.1.0",
    _description = "ProwseTk Lua extension layer"
}

-- ---------------------------------------------------------------------------
-- DOM helpers
-- ---------------------------------------------------------------------------

-- XPath DOM queries. `xpath(document, expr)` returns an array of elements for
-- a node-set, or a scalar (string/number/boolean) otherwise.
lprowsext.dom = {
    xpath = function(document, expression)
        return document:xpath(expression)
    end,
    xpath_strings = function(document, expression)
        return document:xpath_strings(expression)
    end
}

-- ---------------------------------------------------------------------------
-- Endpoint extraction
-- ---------------------------------------------------------------------------

-- Runs heuristic endpoint discovery against a document or session and returns
-- a result object with `openapi_yaml()`, `endpoint_count()`, `endpoints()`,
-- `warnings()`, and `write_openapi_yaml(path)`.
lprowsext.endpoints = {
    extract = function(document_or_session, options)
        return require("lprowsext").endpoints.extract(document_or_session,
                                                      options)
    end
}

-- ---------------------------------------------------------------------------
-- Document extractors
-- ---------------------------------------------------------------------------

-- Document processors keyed by name. Registered processors run against every
-- loaded document; the host wires them into the event loop.
local processors = {}

-- Registers a named document processor. `processor(document)` may return a
-- table; results are accumulated per document.
function lprowsext.register_document_processor(name, processor)
    assert(type(name) == "string", "processor name must be a string")
    assert(type(processor) == "function", "processor must be a function")
    processors[name] = processor
    return true
end

function lprowsext.get_document_processor(name)
    return processors[name]
end

function lprowsext.process_document(document)
    local results = {}
    for name, processor in pairs(processors) do
        local ok, value = pcall(processor, document)
        if ok then
            results[name] = value
        end
    end
    return results
end

-- `extractor.new()` creates an extractor object. `extractor:on_document(fn)`
-- registers the per-document callback; `extractor:run(document)` invokes it.
function lprowsext.extractor.new()
    return require("lprowsext").extractor.new()
end

-- ---------------------------------------------------------------------------
-- WASM plugin handles
-- ---------------------------------------------------------------------------

-- Managed handles only; Lua never receives raw Wasmtime objects. With no WASM
-- runtime linked, `available()` is false and `load()` reports the boundary.
lprowsext.wasm = {
    available = function()
        return require("lprowsext").wasm.available()
    end,
    load = function(path, options)
        return require("lprowsext").wasm.load(path, options)
    end
}

return lprowsext