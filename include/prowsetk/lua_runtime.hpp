#ifndef PROWSETK_LUA_RUNTIME_HPP
#define PROWSETK_LUA_RUNTIME_HPP

#include <memory>
#include <string>
#include <string_view>

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

    LuaResult run(std::string_view code, std::string_view chunk_name = "<string>");
    LuaResult run_file(const std::string& path);

    // Calls a previously loaded global function with no arguments.
    LuaResult call(std::string_view function_name);

    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace prowsetk

#endif  // PROWSETK_LUA_RUNTIME_HPP
