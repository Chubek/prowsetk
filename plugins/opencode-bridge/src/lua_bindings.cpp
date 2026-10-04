// `lopencode` Lua module: managed OpenCode clients for drivers.
//
//   local lopencode = require("lopencode")
//   local client = assert(lopencode.client.new({ base_url = "http://127.0.0.1:4096" }))
//   local id = assert(client:create_session())
//   local answer = assert(client:prompt(id, "Summarize this page"))
//
// An optional second argument to `new` is a host transport callback
// `function(request) -> {status, body[, final_url]}` used by hermetic tests
// and host-mediated routing. Without it, requests go through the default
// socket NetworkClient. The callback lives in the client userdata's uservalue
// so callback/client cycles remain collectable. While a transport call is
// active the client is busy: close/reentry from the callback fails instead of
// deadlocking.

#include "opencode_bridge.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/error.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace prowsetk::plugins::opencode_bridge {
namespace {

constexpr const char* kMeta = "ProwseTk.OpenCodeBridge";
constexpr std::size_t kLuaStringLimit = 1024u * 1024u;

[[noreturn]] void invalid() {
    throw Error(ErrorCode::InvalidArgument, "opencode-bridge: invalid Lua arguments");
}

struct StackRestore {
    lua_State* state;
    int top;
    explicit StackRestore(lua_State* value) : state(value), top(lua_gettop(value)) {}
    ~StackRestore() { lua_settop(state, top); }
};

std::string string_at(lua_State* state, int index, std::size_t limit) {
    if (lua_type(state, index) != LUA_TSTRING) invalid();
    std::size_t size = 0;
    const char* value = lua_tolstring(state, index, &size);
    if (value == nullptr || size > limit) {
        throw Error(ErrorCode::ResourceLimit, "opencode-bridge: Lua string limit exceeded");
    }
    return {value, size};
}

std::string string_field(lua_State* state, int table, const char* name, std::string fallback,
                         std::size_t limit) {
    table = lua_absindex(state, table);
    lua_pushstring(state, name);
    lua_rawget(state, table);
    if (!lua_isnil(state, -1)) fallback = string_at(state, -1, limit);
    lua_pop(state, 1);
    return fallback;
}

bool bool_field(lua_State* state, int table, const char* name, bool fallback) {
    table = lua_absindex(state, table);
    lua_pushstring(state, name);
    lua_rawget(state, table);
    if (!lua_isnil(state, -1)) {
        if (!lua_isboolean(state, -1)) invalid();
        fallback = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);
    return fallback;
}

std::size_t size_field(lua_State* state, int table, const char* name, std::size_t fallback) {
    table = lua_absindex(state, table);
    lua_pushstring(state, name);
    lua_rawget(state, table);
    if (!lua_isnil(state, -1)) {
        if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) < 0) invalid();
        fallback = static_cast<std::size_t>(lua_tointeger(state, -1));
    }
    lua_pop(state, 1);
    return fallback;
}

void push_string(lua_State* state, const char* key, const std::string& value) {
    lua_pushlstring(state, value.data(), value.size());
    lua_setfield(state, -2, key);
}

class LuaNetwork final : public NetworkClient {
public:
    void set_state(lua_State* state) noexcept { state_ = state; }

    HttpResponse send(const HttpRequest& request) override {
        StackRestore restore(state_);
        lua_getuservalue(state_, 1);
        if (!lua_isfunction(state_, -1)) invalid();
        lua_newtable(state_);
        push_string(state_, "method", request.method);
        push_string(state_, "url", request.url);
        push_string(state_, "body", request.body);
        lua_pushinteger(state_, request.timeout_ms);
        lua_setfield(state_, -2, "timeout_ms");
        lua_pushinteger(state_, static_cast<lua_Integer>(request.max_response_bytes));
        lua_setfield(state_, -2, "max_response_bytes");
        lua_newtable(state_);
        for (const auto& [name, value] : request.headers) push_string(state_, name.c_str(), value);
        lua_setfield(state_, -2, "headers");
        if (lua_pcall(state_, 1, 1, 0) != LUA_OK || !lua_istable(state_, -1)) {
            throw Error(ErrorCode::NetworkError, "opencode-bridge: host transport failed");
        }
        const auto status = size_field(state_, -1, "status", 0);
        if (status < 100 || status > 599) invalid();
        HttpResponse response;
        response.status = static_cast<int>(status);
        response.body = string_field(state_, -1, "body", "", request.max_response_bytes);
        response.final_url = string_field(state_, -1, "final_url", "", 4096);
        return response;
    }

private:
    lua_State* state_ = nullptr;
};

struct Client {
    std::unique_ptr<NetworkClient> network;
    std::unique_ptr<OpenCodeClient> bridge;
    bool busy = false;
};
using Handle = std::unique_ptr<Client>;

Handle* handle(lua_State* state) {
    auto* value = static_cast<Handle*>(luaL_testudata(state, 1, kMeta));
    if (value == nullptr) invalid();
    return value;
}

Client& deref(lua_State* state) {
    auto* owner = handle(state);
    if (owner->get() == nullptr) {
        throw Error(ErrorCode::InvalidArgument, "opencode-bridge: client is closed");
    }
    return **owner;
}

template <typename F>
int protect(lua_State* state, F&& function) noexcept {
    const int top = lua_gettop(state);
    try {
        return function();
    } catch (const Error& error) {
        lua_settop(state, top);
        lua_pushnil(state);
        lua_pushstring(state, error.what());
        lua_pushstring(state, to_string(error.code()));
        return 3;
    } catch (...) {
        lua_settop(state, top);
        lua_pushnil(state);
        lua_pushliteral(state, "opencode-bridge: operation failed");
        lua_pushliteral(state, "internal");
        return 3;
    }
}

// Calls method `name` on the value at `index` with no extra args; returns the
// first result as a string (empty when nil). Used to read lprowse session and
// document snapshots through the public Lua API only.
bool call_text_method(lua_State* state, int index, const char* name, std::string& out) {
    index = lua_absindex(state, index);
    StackRestore restore(state);
    if (!lua_istable(state, index) && !lua_isuserdata(state, index)) return false;
    lua_getfield(state, index, name);
    if (!lua_isfunction(state, -1)) return false;
    lua_pushvalue(state, index);
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) return false;
    if (lua_isnil(state, -1)) {
        out.clear();
        return true;
    }
    if (lua_type(state, -1) != LUA_TSTRING) return false;
    std::size_t size = 0;
    const char* value = lua_tolstring(state, -1, &size);
    if (size > kLuaStringLimit) return false;
    out.assign(value, size);
    return true;
}

PageSnapshot snapshot_from_lua(lua_State* state, int index) {
    index = lua_absindex(state, index);
    PageSnapshot snapshot;
    if (lua_istable(state, index)) {
        StackRestore restore(state);
        lua_pushstring(state, "url");
        lua_rawget(state, index);
        if (lua_type(state, -1) == LUA_TSTRING) snapshot.url = string_at(state, -1, kLuaStringLimit);
        lua_pop(state, 1);
        lua_pushstring(state, "title");
        lua_rawget(state, index);
        if (lua_type(state, -1) == LUA_TSTRING) snapshot.title = string_at(state, -1, kLuaStringLimit);
        lua_pop(state, 1);
        lua_pushstring(state, "html");
        lua_rawget(state, index);
        if (lua_type(state, -1) == LUA_TSTRING) snapshot.html = string_at(state, -1, kLuaStringLimit);
        lua_pop(state, 1);
        lua_pushstring(state, "text");
        lua_rawget(state, index);
        if (lua_type(state, -1) == LUA_TSTRING) snapshot.text = string_at(state, -1, kLuaStringLimit);
        lua_pop(state, 1);
        return snapshot;
    }
    // lprowse handles: a session exposes :document(), a document exposes
    // :title()/:url()/:html()/:text() directly. Dereference through the
    // public Lua API only; engine userdata layouts stay private.
    lua_getfield(state, index, "document");
    const bool is_session = lua_isfunction(state, -1);
    lua_pop(state, 1);
    if (is_session) {
        lua_getfield(state, index, "document");
        lua_pushvalue(state, index);
        if (lua_pcall(state, 1, 1, 0) != LUA_OK) invalid();
        if (!lua_isuserdata(state, -1) && !lua_istable(state, -1)) {
            lua_pop(state, 1);
            invalid();
        }
        if (!call_text_method(state, -1, "title", snapshot.title) ||
            !call_text_method(state, -1, "url", snapshot.url) ||
            !call_text_method(state, -1, "html", snapshot.html) ||
            !call_text_method(state, -1, "text", snapshot.text)) {
            lua_pop(state, 1);
            invalid();
        }
        lua_pop(state, 1);
        return snapshot;
    }
    if (!call_text_method(state, index, "title", snapshot.title) ||
        !call_text_method(state, index, "url", snapshot.url) ||
        !call_text_method(state, index, "html", snapshot.html) ||
        !call_text_method(state, index, "text", snapshot.text)) {
        invalid();
    }
    return snapshot;
}

struct BusyGuard {
    bool& busy;
    LuaNetwork* callback;
    BusyGuard(Client& value, lua_State* state)
        : busy(value.busy), callback(dynamic_cast<LuaNetwork*>(value.network.get())) {
        if (busy) throw Error(ErrorCode::InvalidArgument, "opencode-bridge: client is busy");
        busy = true;
        if (callback) callback->set_state(state);
    }
    ~BusyGuard() {
        if (callback) callback->set_state(nullptr);
        busy = false;
    }
};

int create(lua_State* state) noexcept {
    return protect(state, [&] {
        if (!lua_istable(state, 1) || (!lua_isnoneornil(state, 2) && !lua_isfunction(state, 2))) {
            invalid();
        }
        BridgeConfig options = config_from_environment();
        options.base_url = string_field(state, 1, "base_url", options.base_url, 4096);
        options.username = string_field(state, 1, "username", options.username, 4096);
        options.password = string_field(state, 1, "password", options.password, 4096);
        options.allow_remote_http = bool_field(state, 1, "allow_remote_http", options.allow_remote_http);
        const auto timeout = size_field(state, 1, "timeout_ms", static_cast<std::size_t>(options.timeout_ms));
        if (timeout > 300000) invalid();
        options.timeout_ms = static_cast<int>(timeout);
        options.max_input_bytes = size_field(state, 1, "max_input_bytes", options.max_input_bytes);
        options.max_response_bytes =
            size_field(state, 1, "max_response_bytes", options.max_response_bytes);
        options.max_requests = size_field(state, 1, "max_requests", options.max_requests);
        options.api_prefix = string_field(state, 1, "api_prefix", options.api_prefix, 16);
        const auto wait = size_field(state, 1, "prompt_wait_ms",
                                     static_cast<std::size_t>(options.prompt_wait_ms));
        if (wait > 600000) invalid();
        options.prompt_wait_ms = static_cast<int>(wait);
        validate_config(options);

        auto* value = std::construct_at(static_cast<Handle*>(lua_newuserdata(state, sizeof(Handle))));
        luaL_getmetatable(state, kMeta);
        lua_setmetatable(state, -2);
        *value = std::make_unique<Client>();
        if (lua_isfunction(state, 2)) {
            (*value)->network = std::make_unique<LuaNetwork>();
            lua_pushvalue(state, 2);
            lua_setuservalue(state, -2);
        } else {
            (*value)->network = make_socket_network_client();
        }
        (*value)->bridge = std::make_unique<OpenCodeClient>(*(*value)->network, std::move(options));
        return 1;
    });
}

int create_session(lua_State* state) noexcept {
    return protect(state, [&] {
        Client& client = deref(state);
        BusyGuard busy(client, state);
        lua_pushstring(state, client.bridge->create_session().c_str());
        return 1;
    });
}

int prompt(lua_State* state) noexcept {
    return protect(state, [&] {
        Client& client = deref(state);
        const std::string session = string_at(state, 2, 256);
        const std::string text = string_at(state, 3, client.bridge->config().max_input_bytes);
        std::vector<std::string> tools;
        if (!lua_isnoneornil(state, 4)) {
            if (!lua_istable(state, 4)) invalid();
            const auto count = lua_rawlen(state, 4);
            if (count > 16) invalid();
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(state, 4, static_cast<lua_Integer>(i));
                tools.push_back(string_at(state, -1, 256));
                lua_pop(state, 1);
            }
        }
        BusyGuard busy(client, state);
        const std::string answer = client.bridge->prompt(session, text, tools);
        lua_pushlstring(state, answer.data(), answer.size());
        return 1;
    });
}

int prompt_async(lua_State* state) noexcept {
    return protect(state, [&] {
        Client& client = deref(state);
        const std::string session = string_at(state, 2, 256);
        const std::string text = string_at(state, 3, client.bridge->config().max_input_bytes);
        BusyGuard busy(client, state);
        const std::string operation = client.bridge->prompt_async(session, text);
        lua_pushlstring(state, operation.data(), operation.size());
        return 1;
    });
}

int stream_events(lua_State* state) noexcept {
    return protect(state, [&] {
        Client& client = deref(state);
        const std::string session = string_at(state, 2, 256);
        if (!lua_isfunction(state, 3)) invalid();
        BusyGuard busy(client, state);
        const auto events = client.bridge->stream_events(session);
        // Invoke the callback outside any network send, one event at a time.
        // The busy flag stays set so the callback cannot reenter the client.
        int delivered = 0;
        for (const auto& event : events) {
            lua_pushvalue(state, 3);
            lua_pushlstring(state, event.data(), event.size());
            if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
                throw Error(ErrorCode::LuaError, "opencode-bridge: event callback failed");
            }
            ++delivered;
        }
        lua_pushinteger(state, delivered);
        return 1;
    });
}

int abort_session(lua_State* state) noexcept {
    return protect(state, [&] {
        Client& client = deref(state);
        const std::string session = string_at(state, 2, 256);
        BusyGuard busy(client, state);
        client.bridge->abort(session);
        lua_pushboolean(state, 1);
        return 1;
    });
}

int scrape_with_prompt(lua_State* state) noexcept {
    return protect(state, [&] {
        Client& client = deref(state);
        PageSnapshot snapshot = snapshot_from_lua(state, 2);
        const std::string instructions =
            string_at(state, 3, client.bridge->config().max_input_bytes);
        std::string schema;
        if (!lua_isnoneornil(state, 4)) schema = string_at(state, 4, client.bridge->config().max_input_bytes);
        BusyGuard busy(client, state);
        // Convenience path: spawn a dedicated agent session, then run the
        // full sanitize -> prompt -> validate flow through it.
        const std::string session = client.bridge->create_session();
        const std::string answer =
            client.bridge->scrape_with_prompt(session, snapshot, instructions, schema);
        lua_pushlstring(state, answer.data(), answer.size());
        return 1;
    });
}

int close_client(lua_State* state) noexcept {
    return protect(state, [&] {
        auto* owner = handle(state);
        if (*owner && (*owner)->busy) {
            throw Error(ErrorCode::InvalidArgument, "opencode-bridge: client is busy");
        }
        owner->reset();
        lua_pushnil(state);
        lua_setuservalue(state, 1);
        lua_pushboolean(state, 1);
        return 1;
    });
}

int request_count(lua_State* state) noexcept {
    return protect(state, [&] {
        const Client& client = deref(state);
        const auto count = client.bridge->request_count();
        const auto max = static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max());
        if (count > max) invalid();
        lua_pushinteger(state, static_cast<lua_Integer>(count));
        return 1;
    });
}

int build_cleanup_prompt(lua_State* state) noexcept {
    return protect(state, [&] {
        // Pure function: no client, no transport, no budget consumption.
        const std::string endpoints = string_at(state, 1, kLuaStringLimit);
        std::string instructions;
        if (!lua_isnoneornil(state, 2)) instructions = string_at(state, 2, kLuaStringLimit);
        const std::string prompt = build_endpoint_cleanup_prompt(endpoints, instructions);
        lua_pushlstring(state, prompt.data(), prompt.size());
        return 1;
    });
}

int gc(lua_State* state) noexcept {
    auto* value = static_cast<Handle*>(luaL_testudata(state, 1, kMeta));
    if (value != nullptr) std::destroy_at(value);
    return 0;
}

}  // namespace
}  // namespace prowsetk::plugins::opencode_bridge

extern "C" PROWSETK_PLUGIN_EXPORT int luaopen_lopencode(lua_State* state) {
    using namespace prowsetk::plugins::opencode_bridge;
    luaL_newmetatable(state, kMeta);
    lua_pushcfunction(state, gc);
    lua_setfield(state, -2, "__gc");
    lua_pushliteral(state, "ProwseTk.OpenCodeBridge");
    lua_setfield(state, -2, "__metatable");
    const luaL_Reg methods[] = {{"create_session", create_session},
                                {"prompt", prompt},
                                {"prompt_async", prompt_async},
                                {"stream_events", stream_events},
                                {"abort", abort_session},
                                {"scrape_with_prompt", scrape_with_prompt},
                                {"close", close_client},
                                {"request_count", request_count},
                                {nullptr, nullptr}};
    lua_newtable(state);
    luaL_setfuncs(state, methods, 0);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);

    lua_createtable(state, 0, 3);
    // opencode.client.new(...) per AGENTS.md, plus a top-level new alias.
    // build_cleanup_prompt is a pure prompt constructor for endpoint cleanup.
    lua_newtable(state);
    lua_pushcfunction(state, create);
    lua_setfield(state, -2, "new");
    lua_setfield(state, -2, "client");
    lua_pushcfunction(state, create);
    lua_setfield(state, -2, "new");
    lua_pushcfunction(state, build_cleanup_prompt);
    lua_setfield(state, -2, "build_cleanup_prompt");
    lua_pushliteral(state, "0.1.0");
    lua_setfield(state, -2, "_version");
    return 1;
}
