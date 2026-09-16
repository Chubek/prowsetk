#include "prowsetk/javascript_runtime.hpp"

#include "prowsetk/error.hpp"

namespace prowsetk {
namespace {

class NullJavaScriptRuntime : public JavaScriptRuntime {
public:
    ScriptResult evaluate(std::string_view script,
                          const ScriptOptions& options) override {
        (void)script;
        (void)options;
        return ScriptResult{false, {},
                            "JavaScript runtime is not available in this build"};
    }

    void set_global(std::string_view name, std::string_view value) override {
        (void)name;
        (void)value;
    }

    void set_console_handler(ConsoleHandler handler) override {
        (void)handler;
    }

    std::string name() const override { return "null"; }

    CapabilitySet capabilities() const override {
        CapabilitySet capabilities;
        capabilities.set("javascript", ImplementationClass::Unsupported,
                         "no JavaScript engine linked into this build");
        capabilities.set("console", ImplementationClass::Unsupported);
        capabilities.set("fetch", ImplementationClass::Unsupported);
        return capabilities;
    }
};

}  // namespace

std::unique_ptr<JavaScriptRuntime> make_null_javascript_runtime() {
    return std::make_unique<NullJavaScriptRuntime>();
}

}  // namespace prowsetk
