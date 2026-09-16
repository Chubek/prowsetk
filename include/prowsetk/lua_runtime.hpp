#ifndef PROWSETK_LUA_RUNTIME_HPP
#define PROWSETK_LUA_RUNTIME_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "prowsetk/event.hpp"

namespace prowsetk {

class Browser;
class Session;

struct LuaResult {
    bool ok = false;
    std::string error;
};

// Runs Lua automation and extensions. The runtime owns one `lua_State` and
// exposes managed browser handles to Lua; Lua never receives raw engine or
// Wasmtime objects (README "Lua Control Layer").
class LuaRuntime {
public:
    LuaRuntime();
    ~LuaRuntime();

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // True when ProwseTk was built with Lua support.
    static bool available() noexcept;

    // Installs the `lprowse` module bound to `browser`. Must be called before
    // loading scripts that `require("lprowse")`.
    void bind_browser(Browser* browser);

    // The browser bound by bind_browser, or nullptr.
    Browser* bound_browser() noexcept;

    LuaResult run(std::string_view code, std::string_view chunk_name = "<string>");
    LuaResult run_file(const std::string& path);

    // Calls a previously loaded global function with no arguments.
    LuaResult call(std::string_view function_name);

    std::string last_error() const;

    // Registers a Lua event subscription. `registry_ref` is a Lua registry
    // reference owned by the runtime (unreferenced when the subscription is
    // removed or the runtime is destroyed); `alive` lets the owning userdata
    // invalidate the subscription early.
    void add_subscription(EventDispatcher* dispatcher, std::uint64_t subscription_id,
                          int registry_ref,
                          std::shared_ptr<std::atomic<bool>> alive);

    // Removes and unreferences every subscription bound to `dispatcher`. Used
    // when a Lua-owned Browser is garbage collected.
    void release_subscriptions(EventDispatcher* dispatcher);

    // Removes a single subscription by id. Returns whether it was found.
    bool unsubscribe_subscription(std::uint64_t subscription_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace prowsetk

#endif  // PROWSETK_LUA_RUNTIME_HPP