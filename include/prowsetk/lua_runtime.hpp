#ifndef PROWSETK_LUA_RUNTIME_HPP
#define PROWSETK_LUA_RUNTIME_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/event.hpp"

namespace prowsetk {

class Browser;
class Session;

struct LuaResult {
    bool ok = false;
    std::string error;
};

// A named argument passed to a Lua driver entrypoint. `type` mirrors the
// Prowse.toml argument types: "string", "integer", "boolean", "path", "url".
// Values are coerced to native Lua values when the argument table is built.
struct LuaArgument {
    std::string name;
    std::string type = "string";
    std::string value;
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

    // Calls a previously loaded global function (a driver's `main`), passing a
    // single Lua table argument whose fields are `arguments` (values coerced to
    // Lua numbers/booleans/strings per LuaArgument::type). When `return_value`
    // is non-null, the function's first return value is converted with
    // `tostring()` into it (so an integer exit code is retrievable).
    LuaResult call_function(std::string_view function_name,
                            const std::vector<LuaArgument>& arguments,
                            std::string* return_value = nullptr);

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