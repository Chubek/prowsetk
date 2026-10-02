#include "prowsetk/plugins/ai_oracle.hpp"
#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/error.hpp"

#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {
namespace ai = prowsetk::plugins::ai_oracle;
constexpr const char* metatable = "ProwseTk.AiOracle";

[[noreturn]] void invalid() {
    throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "ai-oracle: invalid Lua arguments");
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
    if (size > limit) throw prowsetk::Error(prowsetk::ErrorCode::ResourceLimit, "ai-oracle: Lua string limit exceeded");
    return {value, size};
}

std::string string_field(lua_State* state, int table, const char* name,
                         std::string fallback, std::size_t limit) {
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

class LuaNetwork final : public prowsetk::NetworkClient {
public:
    void set_state(lua_State* state) noexcept { state_ = state; }

    prowsetk::HttpResponse send(const prowsetk::HttpRequest& request) override {
        StackRestore restore(state_);
        // The active method's self is rooted at stack slot 1. Keeping the
        // callback in its uservalue lets Lua collect callback/client cycles;
        // a registry reference would keep those cycles alive indefinitely.
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
            throw prowsetk::Error(prowsetk::ErrorCode::NetworkError, "ai-oracle: host transport failed");
        }
        const auto status = size_field(state_, -1, "status", 0);
        if (status < 100 || status > 599) invalid();
        prowsetk::HttpResponse response;
        response.status = static_cast<int>(status);
        response.body = string_field(state_, -1, "body", "", request.max_response_bytes);
        response.final_url = string_field(state_, -1, "final_url", "", 4096);
        lua_pushliteral(state_, "redirect_chain");
        lua_rawget(state_, -2);
        if (!lua_isnil(state_, -1)) {
            if (!lua_istable(state_, -1)) invalid();
            lua_pushnil(state_);
            if (lua_next(state_, -2)) response.redirect_chain.push_back("redirected");
        }
        return response;
    }

private:
    lua_State* state_ = nullptr;
};

struct Client {
    std::unique_ptr<prowsetk::NetworkClient> network;
    std::unique_ptr<ai::Oracle> oracle;
    std::size_t max_input_bytes = 0;
    bool busy = false;
};
using Handle = std::unique_ptr<Client>;

Handle* handle(lua_State* state) {
    auto* value = static_cast<Handle*>(luaL_testudata(state, 1, metatable));
    if (!value) invalid();
    return value;
}

template <typename F> int protect(lua_State* state, F&& function) noexcept {
    const int top = lua_gettop(state);
    try {
        return function();
    } catch (const prowsetk::Error& error) {
        lua_settop(state, top);
        lua_pushnil(state);
        lua_pushstring(state, error.what());
        lua_pushstring(state, prowsetk::to_string(error.code()));
        return 3;
    } catch (...) {
        lua_settop(state, top);
        lua_pushnil(state);
        lua_pushliteral(state, "ai-oracle: operation failed");
        lua_pushliteral(state, "internal");
        return 3;
    }
}

int create(lua_State* state) noexcept {
    return protect(state, [&] {
        if (!lua_istable(state, 1) || (!lua_isnoneornil(state, 2) && !lua_isfunction(state, 2))) invalid();
        auto options = ai::options_from_environment();
        options.enabled = bool_field(state, 1, "enabled", false);
        options.base_url = string_field(state, 1, "base_url", options.base_url, 4096);
        options.api_key = string_field(state, 1, "api_key", options.api_key, 4096);
        options.model = string_field(state, 1, "model", options.model, 256);
        options.organization = string_field(state, 1, "organization", options.organization, 4096);
        options.project = string_field(state, 1, "project", options.project, 4096);
        const auto timeout = size_field(state, 1, "timeout_ms", static_cast<std::size_t>(options.timeout_ms));
        const auto tokens = size_field(state, 1, "max_output_tokens", options.max_output_tokens);
        if (timeout > 300000 || tokens > 8192) invalid();
        options.timeout_ms = static_cast<int>(timeout);
        options.max_output_tokens = static_cast<std::uint32_t>(tokens);
        options.max_input_bytes = size_field(state, 1, "max_input_bytes", options.max_input_bytes);
        options.max_response_bytes = size_field(state, 1, "max_response_bytes", options.max_response_bytes);
        options.max_requests = size_field(state, 1, "max_requests", options.max_requests);
        ai::validate_options(options);

        auto* value = std::construct_at(static_cast<Handle*>(lua_newuserdata(state, sizeof(Handle))));
        luaL_getmetatable(state, metatable);
        lua_setmetatable(state, -2);
        *value = std::make_unique<Client>();
        if (lua_isfunction(state, 2)) {
            (*value)->network = std::make_unique<LuaNetwork>();
            lua_pushvalue(state, 2);
            lua_setuservalue(state, -2);
        } else {
            (*value)->network = prowsetk::make_socket_network_client();
        }
        (*value)->max_input_bytes = options.max_input_bytes;
        (*value)->oracle = std::make_unique<ai::Oracle>(*(*value)->network, std::move(options));
        return 1;
    });
}

int ask(lua_State* state, const char* default_task, bool default_json) noexcept {
    return protect(state, [&] {
        auto* owner = handle(state);
        if (!*owner) throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "ai-oracle: client is closed");
        if (!lua_istable(state, 2)) invalid();
        auto& client = **owner;
        if (client.busy) throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "ai-oracle: client is busy");
        ai::OracleRequest request;
        request.task = ai::parse_task(string_field(state, 2, "task", default_task, 16));
        request.prompt = string_field(state, 2, "prompt", "", client.max_input_bytes);
        request.context_json = string_field(state, 2, "context_json", "{}", client.max_input_bytes);
        request.html = string_field(state, 2, "html", "", client.max_input_bytes);
        request.page_url = string_field(state, 2, "page_url", "", client.max_input_bytes);
        request.json_output = bool_field(state, 2, "json_output", default_json);
        lua_pushliteral(state, "images");
        lua_rawget(state, 2);
        if (!lua_isnil(state, -1)) {
            if (!lua_istable(state, -1) || lua_rawlen(state, -1) > 4) invalid();
            const auto count = lua_rawlen(state, -1);
            for (std::size_t i = 1; i <= count; ++i) {
                lua_rawgeti(state, -1, static_cast<lua_Integer>(i));
                request.images.push_back(string_at(state, -1, client.max_input_bytes));
                lua_pop(state, 1);
            }
        }
        lua_pop(state, 1);
        struct BusyGuard {
            bool& busy;
            LuaNetwork* callback;
            BusyGuard(Client& value, lua_State* state)
                : busy(value.busy), callback(dynamic_cast<LuaNetwork*>(value.network.get())) {
                busy = true;
                if (callback) callback->set_state(state);
            }
            ~BusyGuard() {
                if (callback) callback->set_state(nullptr);
                busy = false;
            }
        } busy(client, state);
        const auto result = client.oracle->ask(request);
        lua_newtable(state);
        push_string(state, "answer", result.answer);
        push_string(state, "response_id", result.response_id);
        push_string(state, "model", result.model);
        push_string(state, "provenance", result.provenance);
        lua_pushboolean(state, result.advisory);
        lua_setfield(state, -2, "advisory");
        // Lua integers are signed. Refuse metadata that cannot round-trip.
        const auto max = static_cast<std::uint64_t>(std::numeric_limits<lua_Integer>::max());
        if (result.input_tokens > max || result.output_tokens > max) invalid();
        lua_pushinteger(state, static_cast<lua_Integer>(result.input_tokens));
        lua_setfield(state, -2, "input_tokens");
        lua_pushinteger(state, static_cast<lua_Integer>(result.output_tokens));
        lua_setfield(state, -2, "output_tokens");
        return 1;
    });
}

int inquiry(lua_State* state) noexcept { return ask(state, "advice", false); }
int captcha(lua_State* state) noexcept { return ask(state, "captcha", false); }
int crawl(lua_State* state) noexcept { return ask(state, "crawl", true); }

int close(lua_State* state) noexcept {
    return protect(state, [&] {
        auto* owner = handle(state);
        if (*owner && (*owner)->busy) {
            throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "ai-oracle: client is busy");
        }
        owner->reset();
        lua_pushnil(state);
        lua_setuservalue(state, 1);
        lua_pushboolean(state, true);
        return 1;
    });
}

int count(lua_State* state) noexcept {
    return protect(state, [&] {
        auto* owner = handle(state);
        if (!*owner) invalid();
        lua_pushinteger(state, static_cast<lua_Integer>((*owner)->oracle->request_count()));
        return 1;
    });
}

int gc(lua_State* state) noexcept {
    auto* value = static_cast<Handle*>(luaL_testudata(state, 1, metatable));
    if (value) std::destroy_at(value);
    return 0;
}
}  // namespace

extern "C" PROWSETK_PLUGIN_EXPORT int luaopen_ai_oracle(lua_State* state) {
    luaL_newmetatable(state, metatable);
    lua_pushcfunction(state, gc);
    lua_setfield(state, -2, "__gc");
    lua_pushliteral(state, "ProwseTk.AiOracle");
    lua_setfield(state, -2, "__metatable");
    const luaL_Reg methods[] = {{"ask", inquiry}, {"captcha", captcha}, {"crawl", crawl},
                               {"close", close}, {"request_count", count}, {nullptr, nullptr}};
    lua_newtable(state);
    luaL_setfuncs(state, methods, 0);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);
    const luaL_Reg module[] = {{"new", create}, {nullptr, nullptr}};
    luaL_newlib(state, module);
    lua_pushliteral(state, "0.1.0");
    lua_setfield(state, -2, "_version");
    return 1;
}
