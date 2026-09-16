#include "prowsetk/lua_runtime.hpp"

#include <algorithm>
#include <any>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <sstream>

#include "prowsetk/browser.hpp"
#include "prowsetk/document.hpp"
#include "prowsetk/endpoint_extraction.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/xpath.hpp"

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
constexpr const char* kExtractorMeta = "prowsetk.extractor";
constexpr const char* kEndpointResultMeta = "prowsetk.endpoint-result";

struct LuaBrowser {
    Browser* browser = nullptr;
    bool owned = false;
};

struct LuaSession {
    std::shared_ptr<Session>* session = nullptr;
    std::shared_ptr<std::atomic<bool>> alive = nullptr;
};

struct LuaDocument {
    std::shared_ptr<Document>* document = nullptr;
};

struct LuaElement {
    std::shared_ptr<Element>* element = nullptr;
};

struct LuaExtractor {
    int on_document_ref = LUA_NOREF;
    std::shared_ptr<std::atomic<bool>> alive = nullptr;
};

struct LuaEndpointResult {
    EndpointExtractionResult* result = nullptr;
};

// A Lua-registered event subscription. The registry reference is owned by the
// runtime (unreferenced when the subscription is removed or the runtime is
// destroyed); `alive` lets the owning userdata invalidate it early.
struct LuaSubscription {
    EventDispatcher* dispatcher = nullptr;
    SubscriptionId id = 0;
    int ref = LUA_NOREF;
    std::shared_ptr<std::atomic<bool>> alive;
};

LuaRuntime* runtime_from_state(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "prowsetk.lua_runtime");
    auto* runtime = static_cast<LuaRuntime*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return runtime;
}

void push_event_table(lua_State* L, const Event& event) {
    lua_createtable(L, 0, 6);
    lua_pushstring(L, to_string(event.type));
    lua_setfield(L, -2, "type");
    if (!event.url.empty()) {
        lua_pushlstring(L, event.url.c_str(), event.url.size());
        lua_setfield(L, -2, "url");
    }
    if (!event.name.empty()) {
        lua_pushlstring(L, event.name.c_str(), event.name.size());
        lua_setfield(L, -2, "name");
    }
    if (!event.message.empty()) {
        lua_pushlstring(L, event.message.c_str(), event.message.size());
        lua_setfield(L, -2, "message");
    }
    lua_newtable(L);
    for (const auto& [key, value] : event.attributes) {
        lua_pushlstring(L, value.c_str(), value.size());
        lua_setfield(L, -2, key.c_str());
    }
    lua_setfield(L, -2, "attributes");
    lua_pushboolean(L, event.cancelled ? 1 : 0);
    lua_setfield(L, -2, "cancelled");
}

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

LuaExtractor* check_extractor(lua_State* L, int index) {
    return static_cast<LuaExtractor*>(luaL_checkudata(L, index, kExtractorMeta));
}

LuaEndpointResult* check_endpoint_result(lua_State* L, int index) {
    return static_cast<LuaEndpointResult*>(
        luaL_checkudata(L, index, kEndpointResultMeta));
}

void push_element(lua_State* L, const std::shared_ptr<Element>& element);
void push_document(lua_State* L, const std::shared_ptr<Document>& document);
LuaSession* check_session(lua_State* L, int index);
LuaDocument* check_document(lua_State* L, int index);

void push_endpoint_result(lua_State* L, EndpointExtractionResult result) {
    auto* userdata = static_cast<LuaEndpointResult*>(
        lua_newuserdatauv(L, sizeof(LuaEndpointResult), 0));
    ::new (static_cast<void*>(userdata)) LuaEndpointResult{};
    userdata->result = new EndpointExtractionResult(std::move(result));
    luaL_setmetatable(L, kEndpointResultMeta);
}

void push_xpath_value(lua_State* L, const XPathValue& value) {
    switch (value.type) {
        case XPathValueType::String:
            lua_pushlstring(L, value.string_value.c_str(),
                            value.string_value.size());
            return;
        case XPathValueType::Number:
            lua_pushnumber(L, value.number_value);
            return;
        case XPathValueType::Boolean:
            lua_pushboolean(L, value.boolean_value ? 1 : 0);
            return;
        case XPathValueType::NodeSet:
        default:
            lua_createtable(L, static_cast<int>(value.nodes.size()), 0);
            for (std::size_t i = 0; i < value.nodes.size(); ++i) {
                push_element(L, value.nodes[i]);
                lua_rawseti(L, -2, static_cast<int>(i) + 1);
            }
            return;
    }
}

void push_xpath_strings(lua_State* L, const XPathValue& value) {
    lua_createtable(L, static_cast<int>(value.string_values.size()), 0);
    for (std::size_t i = 0; i < value.string_values.size(); ++i) {
        lua_pushlstring(L, value.string_values[i].c_str(),
                        value.string_values[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
}

void push_endpoint_result_table(lua_State* L,
                                const DiscoveredEndpoint& endpoint) {
    lua_createtable(L, 0, 8);
    lua_pushlstring(L, endpoint.url.c_str(), endpoint.url.size());
    lua_setfield(L, -2, "url");
    lua_pushlstring(L, endpoint.path.c_str(), endpoint.path.size());
    lua_setfield(L, -2, "path");
    lua_pushlstring(L, endpoint.method.c_str(), endpoint.method.size());
    lua_setfield(L, -2, "method");
    lua_pushlstring(L, endpoint.source.c_str(), endpoint.source.size());
    lua_setfield(L, -2, "source");
    lua_pushlstring(L, endpoint.discovery_method.c_str(),
                    endpoint.discovery_method.size());
    lua_setfield(L, -2, "discovery_method");
    lua_pushnumber(L, endpoint.confidence);
    lua_setfield(L, -2, "confidence");

    lua_createtable(L, static_cast<int>(endpoint.parameters.size()), 0);
    for (std::size_t i = 0; i < endpoint.parameters.size(); ++i) {
        lua_pushlstring(L, endpoint.parameters[i].c_str(),
                        endpoint.parameters[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_setfield(L, -2, "parameters");

    lua_createtable(L, static_cast<int>(endpoint.notes.size()), 0);
    for (std::size_t i = 0; i < endpoint.notes.size(); ++i) {
        lua_pushlstring(L, endpoint.notes[i].c_str(),
                        endpoint.notes[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_setfield(L, -2, "notes");
}

// Reads an EndpointExtractionOptions table. Missing keys keep defaults.
EndpointExtractionOptions read_endpoint_options(lua_State* L, int index) {
    EndpointExtractionOptions options;
    if (!lua_istable(L, index)) {
        return options;
    }
    const auto read_bool = [&](const char* key, bool& target) {
        lua_getfield(L, index, key);
        if (lua_isboolean(L, -1)) {
            target = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
    };
    const auto read_uint = [&](const char* key, std::uint32_t& target) {
        lua_getfield(L, index, key);
        if (lua_isnumber(L, -1)) {
            target = static_cast<std::uint32_t>(lua_tointeger(L, -1));
        }
        lua_pop(L, 1);
    };
    read_bool("follow_links", options.follow_links);
    read_bool("inspect_scripts", options.inspect_scripts);
    read_bool("observe_network", options.observe_network);
    read_bool("infer_schemas", options.infer_schemas);
    read_bool("include_provenance", options.include_provenance);
    read_bool("redact_secrets", options.redact_secrets);
    read_uint("max_depth", options.max_depth);
    read_uint("max_pages", options.max_pages);

    lua_getfield(L, index, "minimum_confidence");
    if (lua_isnumber(L, -1)) {
        options.minimum_confidence = lua_tonumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "openapi_version");
    if (lua_isstring(L, -1)) {
        options.openapi_version = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    return options;
}

// Accepts a document userdata or a session userdata at `index` and returns the
// underlying Document. Returns nullptr if neither is supplied.
std::shared_ptr<Document> document_from(lua_State* L, int index) {
    if (luaL_testudata(L, index, kDocumentMeta) != nullptr) {
        auto* userdata = check_document(L, index);
        return *userdata->document;
    }
    if (luaL_testudata(L, index, kSessionMeta) != nullptr) {
        auto* userdata = check_session(L, index);
        return (*userdata->session)->document();
    }
    return nullptr;
}

void push_browser(lua_State* L, Browser* browser, bool owned) {
    auto* userdata =
        static_cast<LuaBrowser*>(lua_newuserdatauv(L, sizeof(LuaBrowser), 0));
    ::new (static_cast<void*>(userdata)) LuaBrowser{};
    userdata->browser = browser;
    userdata->owned = owned;
    luaL_setmetatable(L, kBrowserMeta);
}

void push_session(lua_State* L, const std::shared_ptr<Session>& session) {
    auto* userdata =
        static_cast<LuaSession*>(lua_newuserdatauv(L, sizeof(LuaSession), 0));
    ::new (static_cast<void*>(userdata)) LuaSession{};
    userdata->session = new std::shared_ptr<Session>(session);
    userdata->alive = std::make_shared<std::atomic<bool>>(true);
    luaL_setmetatable(L, kSessionMeta);
}

void push_document(lua_State* L, const std::shared_ptr<Document>& document) {
    if (document == nullptr) {
        lua_pushnil(L);
        return;
    }
    auto* userdata =
        static_cast<LuaDocument*>(lua_newuserdatauv(L, sizeof(LuaDocument), 0));
    ::new (static_cast<void*>(userdata)) LuaDocument{};
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
    ::new (static_cast<void*>(userdata)) LuaElement{};
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
    if (userdata->owned && userdata->browser != nullptr) {
        if (LuaRuntime* runtime = runtime_from_state(L)) {
            runtime->release_subscriptions(&userdata->browser->events());
        }
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
    return protect(L, [&]() -> int {
        auto* userdata = check_browser(L, 1);
        luaL_checktype(L, 2, LUA_TUSERDATA);
        if (luaL_testudata(L, 2, kExtractorMeta) == nullptr) {
            luaL_error(L, "install_extension expects an lprowsext.extractor");
            return 0;
        }
        auto* extractor = check_extractor(L, 2);
        if (extractor->on_document_ref == LUA_NOREF) {
            luaL_error(L, "extractor has no on_document callback");
            return 0;
        }
        if (extractor->alive == nullptr) {
            extractor->alive = std::make_shared<std::atomic<bool>>(true);
        }
        const int callback_ref = extractor->on_document_ref;
        const std::shared_ptr<std::atomic<bool>> alive = extractor->alive;
        EventDispatcher* dispatcher = &userdata->browser->events();
        auto handler = [L, callback_ref, alive](Event& event) {
            if (!alive->load()) {
                return;
            }
            std::shared_ptr<Document> document;
            try {
                if (event.payload.type() ==
                    typeid(std::shared_ptr<Document>)) {
                    document =
                        std::any_cast<std::shared_ptr<Document>>(event.payload);
                }
            } catch (const std::bad_any_cast&) {
            }
            if (document == nullptr) {
                return;
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                return;
            }
            push_document(L, document);
            if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                lua_pop(L, 1);
            }
        };
        const SubscriptionId id =
            dispatcher->subscribe(EventType::DocumentCreated,
                                  std::move(handler));
        LuaRuntime* runtime = runtime_from_state(L);
        if (runtime != nullptr) {
            runtime->add_subscription(dispatcher, id, LUA_NOREF, alive);
        }
        lua_pushboolean(L, 1);
        return 1;
    });
}

int browser_new(lua_State* L) {
    return protect(L, [&]() -> int {
        BrowserConfig config;
        if (lua_istable(L, 1)) {
            const auto read_bool = [&](const char* key, bool& target) {
                lua_getfield(L, 1, key);
                if (lua_isboolean(L, -1)) {
                    target = lua_toboolean(L, -1) != 0;
                }
                lua_pop(L, 1);
            };
            const auto read_int = [&](const char* key, int& target) {
                lua_getfield(L, 1, key);
                if (lua_isnumber(L, -1)) {
                    target = static_cast<int>(lua_tointeger(L, -1));
                }
                lua_pop(L, 1);
            };
            const auto read_string = [&](const char* key, std::string& target) {
                lua_getfield(L, 1, key);
                if (lua_isstring(L, -1)) {
                    target = lua_tostring(L, -1);
                }
                lua_pop(L, 1);
            };
            read_bool("javascript", config.javascript);
            read_bool("follow_redirects", config.follow_redirects);
            read_int("max_redirects", config.max_redirects);
            read_int("timeout", config.timeout_ms);
            read_int("timeout_ms", config.timeout_ms);
            read_string("user_agent", config.user_agent);
            read_bool("observe_network", config.observe_network);
            read_string("unsupported_api_behavior",
                        config.unsupported_api_behavior);
        }
        auto* browser = new Browser(std::move(config));
        push_browser(L, browser, true);
        return 1;
    });
}

int session_gc(lua_State* L) {
    auto* userdata = check_session(L, 1);
    if (userdata->alive != nullptr) {
        userdata->alive->store(false);
    }
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

int session_on(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_session(L, 1);
        const char* name = luaL_checkstring(L, 2);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        const std::optional<EventType> type = parse_event_type(name);
        const bool all = std::string_view(name) == "all";
        if (!type.has_value() && !all) {
            luaL_error(L, "unknown event type: %s", name);
            return 0;
        }
        LuaRuntime* runtime = runtime_from_state(L);
        if (runtime == nullptr) {
            luaL_error(L, "Lua runtime is not bound to a host state");
            return 0;
        }
        const std::shared_ptr<std::atomic<bool>> alive =
            userdata->alive != nullptr ? userdata->alive
                                       : std::make_shared<std::atomic<bool>>(true);
        lua_pushvalue(L, 3);
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
        const std::string session_id = (*userdata->session)->id();
        EventDispatcher* dispatcher = &(*userdata->session)->events();
        auto handler = [L, ref, alive, session_id](Event& event) {
            if (!alive->load()) {
                return;
            }
            if (!event.session_id.empty() &&
                event.session_id != session_id) {
                return;
            }
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            if (!lua_isfunction(L, -1)) {
                lua_pop(L, 1);
                return;
            }
            push_event_table(L, event);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                lua_pop(L, 1);
            }
        };
        const SubscriptionId id =
            type.has_value()
                ? dispatcher->subscribe(*type, std::move(handler))
                : dispatcher->subscribe_all(std::move(handler));
        runtime->add_subscription(dispatcher, id, ref, alive);
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        return 1;
    });
}

int session_off(lua_State* L) {
    return protect(L, [&]() -> int {
        const lua_Integer id = luaL_checkinteger(L, 2);
        LuaRuntime* runtime = runtime_from_state(L);
        const bool removed =
            runtime != nullptr &&
            runtime->unsubscribe_subscription(
                static_cast<SubscriptionId>(id));
        lua_pushboolean(L, removed ? 1 : 0);
        return 1;
    });
}

int capabilities_has(lua_State* L) {
    const char* name = luaL_checkstring(L, 2);
    lua_getfield(L, 1, name);
    if (lua_isnil(L, -1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const char* classification = lua_tostring(L, -1);
    const bool supported =
        classification != nullptr &&
        std::string_view(classification) != "unsupported";
    lua_pushboolean(L, supported ? 1 : 0);
    return 1;
}

int session_capabilities(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_session(L, 1);
        const CapabilitySet capabilities = (*userdata->session)->capabilities();
        lua_newtable(L);
        for (const auto& capability : capabilities.all()) {
            lua_pushstring(L, to_string(capability.classification));
            lua_setfield(L, -2, capability.name.c_str());
        }
        lua_pushcfunction(L, capabilities_has);
        lua_setfield(L, -2, "has");
        return 1;
    });
}

int session_request(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_session(L, 1);
        const char* method = luaL_checkstring(L, 2);
        const char* url = luaL_checkstring(L, 3);
        HttpRequest request;
        request.method = method;
        request.url = url;
        if (lua_istable(L, 4)) {
            lua_getfield(L, 4, "headers");
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                        request.headers.emplace_back(lua_tostring(L, -2),
                                                     lua_tostring(L, -1));
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            lua_getfield(L, 4, "body");
            if (lua_isstring(L, -1)) {
                request.body = lua_tostring(L, -1);
            }
            lua_pop(L, 1);
        }
        const HttpResponse response =
            (*userdata->session)->request(std::move(request));
        lua_newtable(L);
        lua_pushinteger(L, response.status);
        lua_setfield(L, -2, "status");
        lua_pushlstring(L, response.body.c_str(), response.body.size());
        lua_setfield(L, -2, "body");
        lua_pushlstring(L, response.final_url.c_str(),
                        response.final_url.size());
        lua_setfield(L, -2, "final_url");
        lua_newtable(L);
        for (const auto& [header, value] : response.headers) {
            lua_pushlstring(L, value.c_str(), value.size());
            lua_setfield(L, -2, header.c_str());
        }
        lua_setfield(L, -2, "headers");
        return 1;
    });
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

int lprowsext_dom_xpath(lua_State* L) {
    return protect(L, [&]() -> int {
        const auto document = document_from(L, 1);
        const char* expression = luaL_checkstring(L, 2);
        if (document == nullptr) {
            lua_pushnil(L);
            return 1;
        }
        push_xpath_value(L, evaluate_xpath(*document, expression));
        return 1;
    });
}

int lprowsext_dom_xpath_on_element(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_element(L, 1);
        const char* expression = luaL_checkstring(L, 2);
        push_xpath_value(L, evaluate_xpath(*(*userdata->element), expression));
        return 1;
    });
}

int lprowsext_dom_xpath_strings(lua_State* L) {
    return protect(L, [&]() -> int {
        const auto document = document_from(L, 1);
        const char* expression = luaL_checkstring(L, 2);
        if (document == nullptr) {
            lua_createtable(L, 0, 0);
            return 1;
        }
        push_xpath_strings(L, evaluate_xpath(*document, expression));
        return 1;
    });
}

int lprowsext_endpoints_extract(lua_State* L) {
    return protect(L, [&]() -> int {
        const auto document = document_from(L, 1);
        if (document == nullptr) {
            luaL_error(L, "endpoints.extract expects a document or session");
            return 0;
        }
        EndpointExtractor extractor(read_endpoint_options(L, 2));
        push_endpoint_result(L, extractor.extract(*document));
        return 1;
    });
}

int endpoint_result_gc(lua_State* L) {
    auto* userdata = check_endpoint_result(L, 1);
    delete userdata->result;
    userdata->result = nullptr;
    return 0;
}

int endpoint_result_openapi_yaml(lua_State* L) {
    auto* userdata = check_endpoint_result(L, 1);
    const std::string& yaml = userdata->result->openapi_yaml;
    lua_pushlstring(L, yaml.c_str(), yaml.size());
    return 1;
}

int endpoint_result_count(lua_State* L) {
    auto* userdata = check_endpoint_result(L, 1);
    lua_pushinteger(L,
                    static_cast<lua_Integer>(userdata->result->endpoints.size()));
    return 1;
}

int endpoint_result_warnings(lua_State* L) {
    auto* userdata = check_endpoint_result(L, 1);
    const auto& warnings = userdata->result->warnings;
    lua_createtable(L, static_cast<int>(warnings.size()), 0);
    for (std::size_t i = 0; i < warnings.size(); ++i) {
        lua_pushlstring(L, warnings[i].c_str(), warnings[i].size());
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

int endpoint_result_endpoints(lua_State* L) {
    auto* userdata = check_endpoint_result(L, 1);
    const auto& endpoints = userdata->result->endpoints;
    lua_createtable(L, static_cast<int>(endpoints.size()), 0);
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        push_endpoint_result_table(L, endpoints[i]);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

int endpoint_result_write_yaml(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_endpoint_result(L, 1);
        const char* path = luaL_checkstring(L, 2);
        std::ofstream stream(path, std::ios::binary);
        if (!stream) {
            lua_pushnil(L);
            lua_pushstring(L, ("cannot open output file: " +
                               std::string(path)).c_str());
            return 2;
        }
        stream << userdata->result->openapi_yaml;
        lua_pushboolean(L, 1);
        return 1;
    });
}

int lprowsext_extractor_new(lua_State* L) {
    auto* userdata =
        static_cast<LuaExtractor*>(lua_newuserdatauv(L, sizeof(LuaExtractor), 0));
    ::new (static_cast<void*>(userdata)) LuaExtractor{};
    userdata->on_document_ref = LUA_NOREF;
    userdata->alive = std::make_shared<std::atomic<bool>>(true);
    luaL_setmetatable(L, kExtractorMeta);
    return 1;
}

int extractor_gc(lua_State* L) {
    auto* userdata = check_extractor(L, 1);
    if (userdata->alive != nullptr) {
        userdata->alive->store(false);
    }
    if (userdata->on_document_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, userdata->on_document_ref);
        userdata->on_document_ref = LUA_NOREF;
    }
    return 0;
}

int extractor_on_document(lua_State* L) {
    auto* userdata = check_extractor(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (userdata->on_document_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, userdata->on_document_ref);
    }
    lua_pushvalue(L, 2);
    userdata->on_document_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushboolean(L, 1);
    return 1;
}

int extractor_run(lua_State* L) {
    return protect(L, [&]() -> int {
        auto* userdata = check_extractor(L, 1);
        if (userdata->on_document_ref == LUA_NOREF) {
            lua_pushnil(L);
            return 1;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, userdata->on_document_ref);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            lua_pushnil(L);
            return 1;
        }
        lua_pushvalue(L, 2);
        const int nargs = 1;
        if (lua_pcall(L, nargs, 1, 0) != LUA_OK) {
            const std::string message =
                lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1)
                                               : "extractor callback failed";
            lua_pop(L, 1);
            luaL_error(L, "%s", message.c_str());
            return 0;
        }
        return 1;
    });
}

int lprowsext_wasm_module_close(lua_State* L);

// Returns the WasmRuntime that Lua scripts should observe: the bound browser's
// runtime when one is bound, otherwise a fresh disabled runtime.
WasmRuntime& effective_wasm_runtime(lua_State* L) {
    static std::unique_ptr<WasmRuntime> fallback;
    if (LuaRuntime* runtime = runtime_from_state(L)) {
        Browser* browser = runtime->bound_browser();
        if (browser != nullptr) {
            return browser->wasm();
        }
    }
    if (fallback == nullptr) {
        fallback = make_wasm_runtime();
    }
    return *fallback;
}

int lprowsext_wasm_available(lua_State* L) {
    lua_pushboolean(L, effective_wasm_runtime(L).enabled() ? 1 : 0);
    return 1;
}

int lprowsext_wasm_load(lua_State* L) {
    return protect(L, [&]() -> int {
        const char* path = luaL_checkstring(L, 1);
        WasmRuntime& runtime = effective_wasm_runtime(L);
        if (!runtime.enabled()) {
            lua_pushnil(L);
            lua_pushliteral(
                L, "WASM plugins are unavailable; this build has no WASM runtime");
            return 2;
        }
        std::shared_ptr<WasmModule> module;
        try {
            module = runtime.load_module(path);
        } catch (const std::exception& error) {
            lua_pushnil(L);
            lua_pushlstring(L, error.what(), std::strlen(error.what()));
            return 2;
        }
        WasmSandboxConfig config;
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "memory_limit_mb");
            if (lua_isnumber(L, -1)) {
                config.max_memory_bytes =
                    static_cast<std::size_t>(lua_tointeger(L, -1)) * 1024u * 1024u;
            }
            lua_pop(L, 1);
            lua_getfield(L, 2, "execution_timeout_ms");
            if (lua_isnumber(L, -1)) {
                config.execution_timeout_ms =
                    static_cast<std::uint32_t>(lua_tointeger(L, -1));
            }
            lua_pop(L, 1);
        }
        // Managed handle only; Lua never receives raw Wasmtime objects.
        lua_createtable(L, 0, 4);
        lua_pushlstring(L, module->name().c_str(), module->name().size());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, path);
        lua_setfield(L, -2, "path");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "loaded");
        lua_pushcfunction(L, lprowsext_wasm_module_close);
        lua_setfield(L, -2, "close");
        return 1;
    });
}

int lprowsext_wasm_module_close(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

// Document processors, keyed by name. They live in the module table's registry
// slot (a plain Lua table) so Lua extensions can register and query them.
int lprowsext_register_document_processor(lua_State* L) {
    return protect(L, [&]() -> int {
        const char* name = luaL_checkstring(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        lua_getglobal(L, "lprowsext");
        lua_getfield(L, -1, "_processors");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, -3, "_processors");
        }
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, name);
        lua_pushboolean(L, 1);
        return 1;
    });
}

int lprowsext_get_document_processor(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_getglobal(L, "lprowsext");
    lua_getfield(L, -1, "_processors");
    if (lua_isnil(L, -1)) {
        lua_pushnil(L);
        return 1;
    }
    lua_getfield(L, -1, name);
    return 1;
}

int lprowsext_process_document(lua_State* L) {
    return protect(L, [&]() -> int {
        lua_getglobal(L, "lprowsext");        // 1
        lua_getfield(L, 1, "_processors");    // 2
        lua_createtable(L, 0, 4);             // 3 (result)
        if (lua_istable(L, 2)) {
            lua_pushnil(L);                   // 4 (key)
            while (lua_next(L, 2) != 0) {     // key=4, value=5
                if (lua_isfunction(L, 5)) {
                    lua_pushvalue(L, 1);      // document argument
                    if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
                        // key=4, retval=5
                        lua_pushvalue(L, 4);  // key
                        lua_pushvalue(L, 5);  // retval
                        lua_settable(L, 3);   // result[key] = retval
                        lua_pop(L, 1);        // pop retval
                    } else {
                        lua_pop(L, 2);        // pop error and value
                    }
                } else {
                    lua_pop(L, 1);            // pop non-function value
                }
                // key remains at index 4 for lua_next to continue
            }
        }
        lua_pushvalue(L, 3);
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
    {"request", session_request},
    {"on", session_on},
    {"off", session_off},
    {"capabilities", session_capabilities},
    {"close", session_close},
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
    {"xpath", lprowsext_dom_xpath_on_element},
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
    {"xpath", lprowsext_dom_xpath},
    {"xpath_strings", lprowsext_dom_xpath_strings},
    {nullptr, nullptr},
};

const luaL_Reg extractor_methods[] = {
    {"on_document", extractor_on_document},
    {"run", extractor_run},
    {nullptr, nullptr},
};

const luaL_Reg endpoint_result_methods[] = {
    {"openapi_yaml", endpoint_result_openapi_yaml},
    {"endpoint_count", endpoint_result_count},
    {"warnings", endpoint_result_warnings},
    {"endpoints", endpoint_result_endpoints},
    {"write_openapi_yaml", endpoint_result_write_yaml},
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
    std::vector<LuaSubscription> subscriptions;
#endif
};

LuaRuntime::LuaRuntime() : impl_(std::make_unique<Impl>()) {
#ifdef PROWSETK_HAVE_LUA
    impl_->state = luaL_newstate();
    if (impl_->state != nullptr) {
        luaL_openlibs(impl_->state);
        // The registry slot lets C functions find the owning runtime.
        lua_pushlightuserdata(impl_->state, this);
        lua_setfield(impl_->state, LUA_REGISTRYINDEX, "prowsetk.lua_runtime");
        register_metatable(impl_->state, kBrowserMeta, browser_methods,
                           browser_gc);
        register_metatable(impl_->state, kSessionMeta, session_methods,
                           session_gc);
        register_metatable(impl_->state, kDocumentMeta, document_methods,
                           document_gc);
        register_metatable(impl_->state, kElementMeta, element_methods,
                           element_gc);
        register_metatable(impl_->state, kExtractorMeta, extractor_methods,
                           extractor_gc);
        register_metatable(impl_->state, kEndpointResultMeta,
                           endpoint_result_methods, endpoint_result_gc);

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

        // ------------------------------------------------------------------
        // lprowsext: the Lua extension layer. Submodules expose XPath DOM
        // queries, endpoint extraction, document extractors, and managed WASM
        // handles. The WASM surface reflects the runtime's availability; with
        // no WASM engine linked, load() reports the capability boundary.
        // ------------------------------------------------------------------
        lua_newtable(impl_->state);  // lprowsext

        lua_newtable(impl_->state);  // lprowsext.dom
        lua_pushcfunction(impl_->state, lprowsext_dom_xpath);
        lua_setfield(impl_->state, -2, "xpath");
        lua_pushcfunction(impl_->state, lprowsext_dom_xpath_strings);
        lua_setfield(impl_->state, -2, "xpath_strings");
        lua_setfield(impl_->state, -2, "dom");

        lua_newtable(impl_->state);  // lprowsext.endpoints
        lua_pushcfunction(impl_->state, lprowsext_endpoints_extract);
        lua_setfield(impl_->state, -2, "extract");
        lua_setfield(impl_->state, -2, "endpoints");

        lua_newtable(impl_->state);  // lprowsext.extractor
        lua_pushcfunction(impl_->state, lprowsext_extractor_new);
        lua_setfield(impl_->state, -2, "new");
        lua_setfield(impl_->state, -2, "extractor");

        lua_newtable(impl_->state);  // lprowsext.wasm
        lua_pushcfunction(impl_->state, lprowsext_wasm_available);
        lua_setfield(impl_->state, -2, "available");
        lua_pushcfunction(impl_->state, lprowsext_wasm_load);
        lua_setfield(impl_->state, -2, "load");
        lua_setfield(impl_->state, -2, "wasm");

        lua_pushcfunction(impl_->state, lprowsext_register_document_processor);
        lua_setfield(impl_->state, -2, "register_document_processor");
        lua_pushcfunction(impl_->state, lprowsext_get_document_processor);
        lua_setfield(impl_->state, -2, "get_document_processor");
        lua_pushcfunction(impl_->state, lprowsext_process_document);
        lua_setfield(impl_->state, -2, "process_document");
        lua_pushliteral(impl_->state, "0.1.0");
        lua_setfield(impl_->state, -2, "_version");

        lua_setglobal(impl_->state, "lprowsext");

        // Make `require("lprowsext")` and its submodules resolve.
        lua_getglobal(impl_->state, "package");
        if (lua_istable(impl_->state, -1)) {
            lua_getfield(impl_->state, -1, "loaded");
            if (lua_istable(impl_->state, -1)) {
                lua_getglobal(impl_->state, "lprowsext");
                lua_setfield(impl_->state, -2, "lprowsext");
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
        for (const auto& subscription : impl_->subscriptions) {
            if (subscription.dispatcher != nullptr) {
                subscription.dispatcher->unsubscribe(subscription.id);
            }
            if (subscription.alive != nullptr) {
                subscription.alive->store(false);
            }
            if (subscription.ref != LUA_NOREF) {
                luaL_unref(impl_->state, LUA_REGISTRYINDEX, subscription.ref);
            }
        }
        impl_->subscriptions.clear();
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

Browser* LuaRuntime::bound_browser() noexcept { return impl_->bound_browser; }

void LuaRuntime::add_subscription(EventDispatcher* dispatcher,
                                  std::uint64_t subscription_id,
                                  int registry_ref,
                                  std::shared_ptr<std::atomic<bool>> alive) {
#ifdef PROWSETK_HAVE_LUA
    impl_->subscriptions.push_back(
        LuaSubscription{dispatcher, static_cast<SubscriptionId>(subscription_id),
                        registry_ref, std::move(alive)});
#endif
}

void LuaRuntime::release_subscriptions(EventDispatcher* dispatcher) {
#ifdef PROWSETK_HAVE_LUA
    if (dispatcher == nullptr) {
        return;
    }
    for (auto it = impl_->subscriptions.begin();
         it != impl_->subscriptions.end();) {
        if (it->dispatcher == dispatcher) {
            dispatcher->unsubscribe(it->id);
            if (it->alive != nullptr) {
                it->alive->store(false);
            }
            if (impl_->state != nullptr && it->ref != LUA_NOREF) {
                luaL_unref(impl_->state, LUA_REGISTRYINDEX, it->ref);
            }
            it = impl_->subscriptions.erase(it);
        } else {
            ++it;
        }
    }
#endif
}

bool LuaRuntime::unsubscribe_subscription(std::uint64_t subscription_id) {
#ifdef PROWSETK_HAVE_LUA
    const auto it = std::find_if(
        impl_->subscriptions.begin(), impl_->subscriptions.end(),
        [subscription_id](const LuaSubscription& subscription) {
            return static_cast<std::uint64_t>(subscription.id) ==
                   subscription_id;
        });
    if (it == impl_->subscriptions.end()) {
        return false;
    }
    if (it->dispatcher != nullptr) {
        it->dispatcher->unsubscribe(it->id);
    }
    if (it->alive != nullptr) {
        it->alive->store(false);
    }
    if (impl_->state != nullptr && it->ref != LUA_NOREF) {
        luaL_unref(impl_->state, LUA_REGISTRYINDEX, it->ref);
    }
    impl_->subscriptions.erase(it);
    return true;
#else
    (void)subscription_id;
    return false;
#endif
}

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

LuaResult LuaRuntime::call_function(std::string_view function_name,
                                    const std::vector<LuaArgument>& arguments,
                                    std::string* return_value) {
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
    lua_newtable(impl_->state);
    for (const auto& argument : arguments) {
        lua_pushstring(impl_->state, argument.name.c_str());
        if (argument.type == "integer") {
            lua_pushinteger(
                impl_->state,
                static_cast<lua_Integer>(
                    std::strtoll(argument.value.c_str(), nullptr, 10)));
        } else if (argument.type == "boolean") {
            bool parsed = false;
            const std::string lower =
                [&]() {
                    std::string value = argument.value;
                    std::transform(value.begin(), value.end(), value.begin(),
                                   [](char c) {
                                       return static_cast<char>(
                                           std::tolower(
                                               static_cast<unsigned char>(c)));
                                   });
                    return value;
                }();
            if (lower == "true" || lower == "1" || lower == "yes" ||
                lower == "on") {
                parsed = true;
            }
            lua_pushboolean(impl_->state, parsed ? 1 : 0);
        } else {
            lua_pushstring(impl_->state, argument.value.c_str());
        }
        lua_settable(impl_->state, -3);
    }
    const int results = return_value != nullptr ? 1 : 0;
    if (lua_pcall(impl_->state, 1, results, 0) != LUA_OK) {
        impl_->last_error = lua_tostring(impl_->state, -1) != nullptr
                                ? lua_tostring(impl_->state, -1)
                                : "Lua runtime error";
        lua_pop(impl_->state, 1);
        return LuaResult{false, impl_->last_error};
    }
    if (return_value != nullptr) {
        if (lua_isnil(impl_->state, -1)) {
            return_value->clear();
        } else if (lua_isstring(impl_->state, -1) ||
                   lua_isnumber(impl_->state, -1)) {
            *return_value = lua_tostring(impl_->state, -1);
        } else if (lua_isboolean(impl_->state, -1)) {
            *return_value =
                lua_toboolean(impl_->state, -1) != 0 ? "true" : "false";
        } else {
            lua_pushvalue(impl_->state, -1);
            luaL_tolstring(impl_->state, -1, nullptr);
            *return_value = lua_tostring(impl_->state, -1);
            lua_pop(impl_->state, 2);
        }
        lua_pop(impl_->state, 1);
    }
    impl_->last_error.clear();
    return LuaResult{true, {}};
#else
    (void)function_name;
    (void)arguments;
    (void)return_value;
    impl_->last_error = "ProwseTk was built without Lua support";
    return LuaResult{false, impl_->last_error};
#endif
}

std::string LuaRuntime::last_error() const { return impl_->last_error; }

}  // namespace prowsetk
