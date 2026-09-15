#include "prowsetk/lua_runtime.hpp"

#include <fstream>
#include <sstream>

#include "prowsetk/browser.hpp"
#include "prowsetk/document.hpp"
#include "prowsetk/error.hpp"

#ifdef PROWSETK_HAVE_LUA
#include <lua.hpp>
#endif

namespace prowsetk {

#ifdef PROWSETK_HAVE_LUA
namespace {

constexpr const char* kBrowserMeta = "prowsetk.browser";
constexpr const char* kSessionMeta = "prowsetk.session";
constexpr const char* kDocumentMeta = "prowsetk.document";
constexpr const char* kElementMeta = "prowsetk.element";

struct LuaBrowser {
    Browser* browser = nullptr;
    bool owned = false;
};

struct LuaSession {
    std::shared_ptr<Session>* session = nullptr;
};

struct LuaDocument {
    std::shared_ptr<Document>* document = nullptr;
};

struct LuaElement {
    std::shared_ptr<Element>* element = nullptr;
};

LuaBrowser* check_browser(lua_State* L, int index) {
    return static_cast<LuaBrowser*>(luaL_checkudata(L, index, kBrowserMeta));
}

LuaSession* check_session(lua_State* L, int index) {
    return static_cast<LuaSession*>(luaL_checkudata(L, index, kSessionMeta));
}

LuaDocument* check_document(lua_State* L, int index) {
    return static_cast<LuaDocument*>(luaL_checkudata(L, index, kDocumentMeta));
}

LuaElement* check_element(lua_State* L, int index) {
    return static_cast<LuaElement*>(luaL_checkudata(L, index, kElementMeta));
}

void push_browser(lua_State* L, Browser* browser, bool owned) {
    auto* userdata =
        static_cast<LuaBrowser*>(lua_newuserdatauv(L, sizeof(LuaBrowser), 0));
    userdata->browser = browser;
    userdata->owned = owned;
    luaL_setmetatable(L, kBrowserMeta);
}

void push_session(lua_State* L, const std::shared_ptr<Session>& session) {
    auto* userdata =
        static_cast<LuaSession*>(lua_newuserdatauv(L, sizeof(LuaSession), 0));
    userdata->session = new std::shared_ptr<Session>(session);
    luaL_setmetatable(L, kSessionMeta);
}

void push_document(lua_State* L, const std::shared_ptr<Document>& document) {
    if (document == nullptr) {
        lua_pushnil(L);
        return;
    }
    auto* userdata =
        static_cast<LuaDocument*>(lua_newuserdatauv(L, sizeof(LuaDocument), 0));
    userdata->document = new std::shared_ptr<Document>(document);
    luaL_setmetatable(L, kDocumentMeta);
}

void push_element(lua_State* L, const std::shared_ptr<Element>& element) {
    if (element == nullptr) {
        lua_pushnil(L);
        return;
    }
    auto* userdata =
        static_cast<LuaElement*>(lua_newuserdatauv(L, sizeof(LuaElement), 0));
    userdata->element = new std::shared_ptr<Element>(element);
    luaL_setmetatable(L, kElementMeta);
}

template <typename F>
int protect(lua_State* L, F&& function) {
    try {
        return function();
    } catch (const std::exception& error) {
        lua_pushstring(L, error.what());
        return lua_error(L);
    } catch (...) {
        lua_pushstring(L, "unknown error");
        return lua_error(L);
    }
}

int browser_gc(lua_State* L) {
    auto* userdata = check_browser(L, 1);
    if (userdata->owned) {
        delete userdata->browser;
    }
    userdata->browser = nullptr;
    return 0;
}

int browser_create_session(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_browser(L, 1);
        SessionConfig config;
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "isolate_storage");
            if (lua_isboolean(L, -1)) {
                config.isolate_storage = lua_toboolean(L, -1) != 0;
            }
            lua_pop(L, 1);
        }
        push_session(L, userdata->browser->create_session(config));
        return 1;
    });
}

int browser_install_extension(lua_State* L) {
    (void)L;
    lua_pushboolean(L, 1);
    return 1;
}

int browser_new(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* browser = new Browser();
        push_browser(L, browser, true);
        return 1;
    });
}

int session_gc(lua_State* L) {
    auto* userdata = check_session(L, 1);
    delete userdata->session;
    userdata->session = nullptr;
    return 0;
}

int session_navigate(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_session(L, 1);
        const char* url = luaL_checkstring(L, 2);
        (*userdata->session)->navigate(url);
        lua_pushboolean(L, 1);
        return 1;
    });
}

int session_load_html(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_session(L, 1);
        const char* html = luaL_checkstring(L, 2);
        const char* base = lua_isnoneornil(L, 3) ? nullptr : luaL_checkstring(L, 3);
        (*userdata->session)->load_html(html, base != nullptr ? base : "");
        lua_pushboolean(L, 1);
        return 1;
    });
}

int session_document(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_session(L, 1);
        push_document(L, (*userdata->session)->document());
        return 1;
    });
}

int session_evaluate_js(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_session(L, 1);
        const char* script = luaL_checkstring(L, 2);
        const std::string result = (*userdata->session)->evaluate_js(script);
        lua_pushlstring(L, result.c_str(), result.size());
        return 1;
    });
}

int session_current_url(lua_State* L) {
    auto* userdata = check_session(L, 1);
    const std::string& url = (*userdata->session)->current_url();
    lua_pushlstring(L, url.c_str(), url.size());
    return 1;
}

int session_set_header(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_session(L, 1);
        const char* name = luaL_checkstring(L, 2);
        const char* value = luaL_checkstring(L, 3);
        (*userdata->session)->set_header(name, value);
        lua_pushboolean(L, 1);
        return 1;
    });
}

int session_close(lua_State* L) {
    auto* userdata = check_session(L, 1);
    (*userdata->session)->close();
    return 0;
}

int document_gc(lua_State* L) {
    auto* userdata = check_document(L, 1);
    delete userdata->document;
    userdata->document = nullptr;
    return 0;
}

int document_title(lua_State* L) {
    auto* userdata = check_document(L, 1);
    const std::string title = (*userdata->document)->title();
    lua_pushlstring(L, title.c_str(), title.size());
    return 1;
}

int document_url(lua_State* L) {
    auto* userdata = check_document(L, 1);
    const std::string url = (*userdata->document)->url();
    lua_pushlstring(L, url.c_str(), url.size());
    return 1;
}

int document_text(lua_State* L) {
    auto* userdata = check_document(L, 1);
    const std::string text = (*userdata->document)->text();
    lua_pushlstring(L, text.c_str(), text.size());
    return 1;
}

int document_html(lua_State* L) {
    auto* userdata = check_document(L, 1);
    const std::string html = (*userdata->document)->html();
    lua_pushlstring(L, html.c_str(), html.size());
    return 1;
}

int document_query_selector(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_document(L, 1);
        const char* selector = luaL_checkstring(L, 2);
        push_element(L, (*userdata->document)->query_selector(selector));
        return 1;
    });
}

int document_query_selector_all(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_document(L, 1);
        const char* selector = luaL_checkstring(L, 2);
        const auto elements =
            (*userdata->document)->query_selector_all(selector);
        lua_createtable(L, static_cast<int>(elements.size()), 0);
        int index = 1;
        for (const auto& element : elements) {
            push_element(L, element);
            lua_rawseti(L, -2, index++);
        }
        return 1;
    });
}

int document_get_element_by_id(lua_State* L) {
    auto* userdata = check_document(L, 1);
    const char* id = luaL_checkstring(L, 2);
    push_element(L, (*userdata->document)->get_element_by_id(id));
    return 1;
}

int document_metadata(lua_State* L) {
    auto* userdata = check_document(L, 1);
    const auto metadata = (*userdata->document)->metadata();
    lua_createtable(L, 0, static_cast<int>(metadata.size()));
    for (const auto& [key, value] : metadata) {
        lua_pushlstring(L, value.c_str(), value.size());
        lua_setfield(L, -2, key.c_str());
    }
    return 1;
}

int document_links(lua_State* L) {
    auto* userdata = check_document(L, 1);
    const auto links = (*userdata->document)->links();
    lua_createtable(L, static_cast<int>(links.size()), 0);
    int index = 1;
    for (const auto& link : links) {
        push_element(L, link);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

int element_gc(lua_State* L) {
    auto* userdata = check_element(L, 1);
    delete userdata->element;
    userdata->element = nullptr;
    return 0;
}

int element_tag_name(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const std::string name = (*userdata->element)->tag_name();
    lua_pushlstring(L, name.c_str(), name.size());
    return 1;
}

int element_id(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const std::string id = (*userdata->element)->id();
    lua_pushlstring(L, id.c_str(), id.size());
    return 1;
}

int element_class_name(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const std::string name = (*userdata->element)->class_name();
    lua_pushlstring(L, name.c_str(), name.size());
    return 1;
}

int element_attribute(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const std::string value = (*userdata->element)->attribute(name);
    lua_pushlstring(L, value.c_str(), value.size());
    return 1;
}

int element_has_attribute(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const char* name = luaL_checkstring(L, 2);
    lua_pushboolean(L, (*userdata->element)->has_attribute(name) ? 1 : 0);
    return 1;
}

int element_set_attribute(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const char* name = luaL_checkstring(L, 2);
    const char* value = luaL_checkstring(L, 3);
    (*userdata->element)->set_attribute(name, value);
    lua_pushboolean(L, 1);
    return 1;
}

int element_text(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const std::string text = (*userdata->element)->text();
    lua_pushlstring(L, text.c_str(), text.size());
    return 1;
}

int element_inner_html(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const std::string html = (*userdata->element)->inner_html();
    lua_pushlstring(L, html.c_str(), html.size());
    return 1;
}

int element_outer_html(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const std::string html = (*userdata->element)->outer_html();
    lua_pushlstring(L, html.c_str(), html.size());
    return 1;
}

int element_value(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const std::string value = (*userdata->element)->value();
    lua_pushlstring(L, value.c_str(), value.size());
    return 1;
}

int element_set_value(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const char* value = luaL_checkstring(L, 2);
    (*userdata->element)->set_value(value);
    lua_pushboolean(L, 1);
    return 1;
}

int element_query_selector(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_element(L, 1);
        const char* selector = luaL_checkstring(L, 2);
        push_element(L, (*userdata->element)->query_selector(selector));
        return 1;
    });
}

int element_query_selector_all(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_element(L, 1);
        const char* selector = luaL_checkstring(L, 2);
        const auto elements = (*userdata->element)->query_selector_all(selector);
        lua_createtable(L, static_cast<int>(elements.size()), 0);
        int index = 1;
        for (const auto& element : elements) {
            push_element(L, element);
            lua_rawseti(L, -2, index++);
        }
        return 1;
    });
}

int element_children(lua_State* L) {
    auto* userdata = check_element(L, 1);
    const auto children = (*userdata->element)->children();
    lua_createtable(L, static_cast<int>(children.size()), 0);
    int index = 1;
    for (const auto& child : children) {
        push_element(L, child);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

int element_parent(lua_State* L) {
    auto* userdata = check_element(L, 1);
    push_element(L, (*userdata->element)->parent());
    return 1;
}

int element_matches(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_element(L, 1);
        const char* selector = luaL_checkstring(L, 2);
        lua_pushboolean(L, (*userdata->element)->matches(selector) ? 1 : 0);
        return 1;
    });
}

const luaL_Reg browser_methods[] = {
    {"create_session", browser_create_session},
    {"install_extension", browser_install_extension},
    {nullptr, nullptr},
};

const luaL_Reg session_methods[] = {
    {"navigate", session_navigate},
    {"load_html", session_load_html},
    {"document", session_document},
    {"evaluate_js", session_evaluate_js},
    {"current_url", session_current_url},
    {"set_header", session_set_header},
    {"close", session_close},
    {nullptr, nullptr},
};

const luaL_Reg document_methods[] = {
    {"title", document_title},
    {"url", document_url},
    {"text", document_text},
    {"html", document_html},
    {"query_selector", document_query_selector},
    {"query_selector_all", document_query_selector_all},
    {"get_element_by_id", document_get_element_by_id},
    {"metadata", document_metadata},
    {"links", document_links},
    {nullptr, nullptr},
};

const luaL_Reg element_methods[] = {
    {"tag_name", element_tag_name},
    {"id", element_id},
    {"class_name", element_class_name},
    {"attribute", element_attribute},
    {"has_attribute", element_has_attribute},
    {"set_attribute", element_set_attribute},
    {"text", element_text},
    {"inner_html", element_inner_html},
    {"html", element_outer_html},
    {"value", element_value},
    {"set_value", element_set_value},
    {"query_selector", element_query_selector},
    {"query_selector_all", element_query_selector_all},
    {"children", element_children},
    {"parent", element_parent},
    {"matches", element_matches},
    {nullptr, nullptr},
};

void register_metatable(lua_State* L, const char* name, const luaL_Reg* methods,
                        lua_CFunction gc) {
    luaL_newmetatable(L, name);
    lua_pushcfunction(L, gc);
    lua_setfield(L, -2, "__gc");
    luaL_setfuncs(L, methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

}  // namespace
#endif  // PROWSETK_HAVE_LUA

struct LuaRuntime::Impl {
    Browser* bound_browser = nullptr;
    std::string last_error;
#ifdef PROWSETK_HAVE_LUA
    lua_State* state = nullptr;
#endif
};

LuaRuntime::LuaRuntime() : impl_(std::make_unique<Impl>()) {
#ifdef PROWSETK_HAVE_LUA
    impl_->state = luaL_newstate();
    if (impl_->state != nullptr) {
        luaL_openlibs(impl_->state);
        register_metatable(impl_->state, kBrowserMeta, browser_methods,
                           browser_gc);
        register_metatable(impl_->state, kSessionMeta, session_methods,
                           session_gc);
        register_metatable(impl_->state, kDocumentMeta, document_methods,
                           document_gc);
        register_metatable(impl_->state, kElementMeta, element_methods,
                           element_gc);

        lua_newtable(impl_->state);
        lua_newtable(impl_->state);
        lua_pushcfunction(impl_->state, browser_new);
        lua_setfield(impl_->state, -2, "new");
        lua_setfield(impl_->state, -2, "browser");
        lua_setglobal(impl_->state, "lprowse");

        // Make `require("lprowse")` resolve to the same module table.
        lua_getglobal(impl_->state, "package");
        if (lua_istable(impl_->state, -1)) {
            lua_getfield(impl_->state, -1, "loaded");
            if (lua_istable(impl_->state, -1)) {
                lua_getglobal(impl_->state, "lprowse");
                lua_setfield(impl_->state, -2, "lprowse");
            }
            lua_pop(impl_->state, 1);
        }
        lua_pop(impl_->state, 1);
    }
#endif
}

LuaRuntime::~LuaRuntime() {
#ifdef PROWSETK_HAVE_LUA
    if (impl_->state != nullptr) {
        lua_close(impl_->state);
        impl_->state = nullptr;
    }
#endif
}

bool LuaRuntime::available() noexcept {
#ifdef PROWSETK_HAVE_LUA
    return true;
#else
    return false;
#endif
}

void LuaRuntime::bind_browser(Browser* browser) { impl_->bound_browser = browser; }

LuaResult LuaRuntime::run(std::string_view code, std::string_view chunk_name) {
#ifdef PROWSETK_HAVE_LUA
    if (impl_->state == nullptr) {
        impl_->last_error = "Lua state is not initialized";
        return LuaResult{false, impl_->last_error};
    }
    const std::string name(chunk_name);
    if (luaL_loadbuffer(impl_->state, code.data(), code.size(),
                        name.c_str()) != LUA_OK) {
        impl_->last_error =
            lua_tostring(impl_->state, -1) != nullptr
                ? lua_tostring(impl_->state, -1)
                : "Lua compile error";
        lua_pop(impl_->state, 1);
        return LuaResult{false, impl_->last_error};
    }
    if (lua_pcall(impl_->state, 0, LUA_MULTRET, 0) != LUA_OK) {
        impl_->last_error = lua_tostring(impl_->state, -1) != nullptr
                                ? lua_tostring(impl_->state, -1)
                                : "Lua runtime error";
        lua_pop(impl_->state, 1);
        return LuaResult{false, impl_->last_error};
    }
    impl_->last_error.clear();
    return LuaResult{true, {}};
#else
    (void)code;
    (void)chunk_name;
    impl_->last_error = "ProwseTk was built without Lua support";
    return LuaResult{false, impl_->last_error};
#endif
}

LuaResult LuaRuntime::run_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        impl_->last_error = "cannot open Lua file: " + path;
        return LuaResult{false, impl_->last_error};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return run(buffer.str(), path);
}

LuaResult LuaRuntime::call(std::string_view function_name) {
#ifdef PROWSETK_HAVE_LUA
    if (impl_->state == nullptr) {
        impl_->last_error = "Lua state is not initialized";
        return LuaResult{false, impl_->last_error};
    }
    lua_getglobal(impl_->state, std::string(function_name).c_str());
    if (!lua_isfunction(impl_->state, -1)) {
        lua_pop(impl_->state, 1);
        impl_->last_error = "Lua global is not a function: " +
                            std::string(function_name);
        return LuaResult{false, impl_->last_error};
    }
    if (lua_pcall(impl_->state, 0, 0, 0) != LUA_OK) {
        impl_->last_error = lua_tostring(impl_->state, -1) != nullptr
                                ? lua_tostring(impl_->state, -1)
                                : "Lua runtime error";
        lua_pop(impl_->state, 1);
        return LuaResult{false, impl_->last_error};
    }
    impl_->last_error.clear();
    return LuaResult{true, {}};
#else
    (void)function_name;
    impl_->last_error = "ProwseTk was built without Lua support";
    return LuaResult{false, impl_->last_error};
#endif
}

std::string LuaRuntime::last_error() const { return impl_->last_error; }

}  // namespace prowsetk
