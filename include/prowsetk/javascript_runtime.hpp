#ifndef PROWSETK_JAVASCRIPT_RUNTIME_HPP
#define PROWSETK_JAVASCRIPT_RUNTIME_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "prowsetk/capability.hpp"

namespace prowsetk {

struct ScriptOptions {
    int timeout_ms = 5000;
    std::size_t memory_limit_bytes = std::size_t{16} * 1024u * 1024u;
};

struct ScriptResult {
    bool ok = false;
    std::string value;
    std::string error;
};

// Executes page JavaScript. JavaScript is the page-scripting runtime only; it
// is never an extension mechanism (README "JavaScript Execution").
class JavaScriptRuntime {
public:
    virtual ~JavaScriptRuntime() = default;

    virtual ScriptResult evaluate(std::string_view script,
                                  const ScriptOptions& options = {}) = 0;
    virtual void set_global(std::string_view name, std::string_view value) = 0;
    virtual std::string name() const = 0;
    virtual CapabilitySet capabilities() const = 0;
};

// Reports JavaScript as unsupported. Used when no JavaScript engine is linked
// into the build so that automation still runs deterministically.
std::unique_ptr<JavaScriptRuntime> make_null_javascript_runtime();

// Returns the QuickJS-backed runtime when ProwseTk is built with
// PROWSETK_HAVE_QUICKJS, otherwise returns the null runtime.
std::unique_ptr<JavaScriptRuntime> make_javascript_runtime();

}  // namespace prowsetk

#endif  // PROWSETK_JAVASCRIPT_RUNTIME_HPP
