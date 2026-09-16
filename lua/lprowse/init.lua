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

-- `prowse.browser.new([config])` creates a Browser. The native browser handle
-- exposes `create_session([config])` and `install_extension(extractor)`.
lprowse.browser = {
    new = native_unavailable
}

-- Sessions created by the native module provide:
--
--   navigate(url), load_html(html [, base_url]), document(), current_url(),
--   evaluate_js(script), request(method, url [, options]),
--   set_header(name, value), headers(), clear_headers(),
--   on(event, callback), off(subscription), capabilities(), and close().
--
-- `request` returns `{ status, body, final_url, headers }`. Capability values
-- are classifications such as `supported`, `dummy`, or `unsupported`; use
-- `session:capabilities():has(name)` when only support status is needed.

-- Documents provide title, URL, markup, text, selector, discovery, and DOM
-- construction methods:
--
--   title(), url(), text(), html(), root(), query_selector(selector),
--   query_selector_all(selector), get_element_by_id(id),
--   get_elements_by_tag_name(tag), metadata(), links(), forms(), scripts(),
--   resource_urls(), create_element(tag), xpath(expression), and
--   xpath_strings(expression).
--
-- Elements provide inspection, traversal, query, form-value, and mutation
-- methods: tag_name, id, class_name, attribute, attributes, has_attribute,
-- set_attribute, remove_attribute, text, inner_html, html, value, set_value,
-- query_selector, query_selector_all, children, parent, first_child,
-- next_sibling, previous_sibling, matches, append_child, remove_child,
-- set_text, and xpath.

return lprowse
