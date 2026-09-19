#ifndef PROWSETK_JAVASCRIPT_RUNTIME_HPP
#define PROWSETK_JAVASCRIPT_RUNTIME_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "prowsetk/capability.hpp"

namespace prowsetk {

struct ScriptOptions {
    int timeout_ms = 5000;
    std::size_t memory_limit_bytes = std::size_t{16} * 1024u * 1024u;
    std::size_t max_microtask_jobs = 10000;
};

struct ScriptResult {
    bool ok = false;
    std::string value;
    std::string error;
};

// A console message forwarded from page JavaScript (README "Events and Hooks":
// console message). `level` is one of "log", "info", "warn", "error", "debug".
struct ConsoleMessage {
    std::string level;
    std::string text;
};

using ConsoleHandler = std::function<void(const ConsoleMessage&)>;

// Minimal document host exposed to page JavaScript. This is deliberately small:
// JavaScript remains page scripting only, while DOM access is mediated through
// Session-owned callbacks instead of exposing engine internals or C++ objects.
class DocumentScriptHost {
public:
    virtual ~DocumentScriptHost() = default;

    virtual std::string document_element_class_name() const = 0;
    virtual void set_document_element_class_name(std::string_view value) = 0;
    virtual std::string document_url() const = 0;
};

// Executes page JavaScript. JavaScript is the page-scripting runtime only; it
// is never an extension mechanism (README "JavaScript Execution").
class JavaScriptRuntime {
public:
    virtual ~JavaScriptRuntime() = default;

    virtual ScriptResult evaluate(std::string_view script,
                                  const ScriptOptions& options = {}) = 0;
    virtual ScriptResult run_microtasks(const ScriptOptions& options = {});
    virtual bool has_pending_microtasks() const;
    virtual void set_global(std::string_view name, std::string_view value) = 0;

    // Installs the handler invoked for every console.* call made by page
    // scripts. A session wires it to emit EventType::Console events.
    virtual void set_console_handler(ConsoleHandler handler) = 0;
    virtual void set_document_host(DocumentScriptHost* host);

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
