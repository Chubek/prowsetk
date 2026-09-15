#include "prowsetk/javascript_runtime.hpp"

#ifdef PROWSETK_HAVE_QUICKJS

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <quickjs.h>

namespace prowsetk {
namespace {

struct RuntimeDeleter {
    void operator()(JSRuntime* runtime) const noexcept {
        if (runtime != nullptr) JS_FreeRuntime(runtime);
    }
};

struct ContextDeleter {
    void operator()(JSContext* context) const noexcept {
        if (context != nullptr) JS_FreeContext(context);
    }
};

using RuntimePtr = std::unique_ptr<JSRuntime, RuntimeDeleter>;
using ContextPtr = std::unique_ptr<JSContext, ContextDeleter>;

struct Deadline {
    std::chrono::steady_clock::time_point expires_at;
};

int interrupt_at_deadline(JSRuntime*, void* opaque) {
    const auto* deadline = static_cast<const Deadline*>(opaque);
    return std::chrono::steady_clock::now() >= deadline->expires_at ? 1 : 0;
}

std::string value_to_string(JSContext* context, JSValueConst value) {
    std::size_t length = 0;
    const char* text = JS_ToCStringLen(context, &length, value);
    if (text == nullptr) return {};
    std::string result(text, length);
    JS_FreeCString(context, text);
    return result;
}

std::string take_exception(JSContext* context) {
    JSValue exception = JS_GetException(context);
    std::string message = value_to_string(context, exception);
    if (JS_IsError(exception)) {
        JSValue stack = JS_GetPropertyStr(context, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            std::string stack_text = value_to_string(context, stack);
            if (!stack_text.empty()) {
                if (!message.empty()) message.push_back('\n');
                message += stack_text;
            }
        }
        JS_FreeValue(context, stack);
    }
    JS_FreeValue(context, exception);
    return message.empty() ? "JavaScript evaluation failed" : message;
}

class QuickJavaScriptRuntime final : public JavaScriptRuntime {
public:
    QuickJavaScriptRuntime()
        : runtime_(JS_NewRuntime()),
          context_(runtime_ != nullptr ? JS_NewContext(runtime_.get()) : nullptr) {
        if (runtime_ == nullptr || context_ == nullptr) throw std::bad_alloc();
        JS_SetCanBlock(runtime_.get(), false);
    }

    ScriptResult evaluate(std::string_view script,
                          const ScriptOptions& options) override {
        if (options.timeout_ms <= 0) {
            return {false, {}, "JavaScript timeout must be positive"};
        }
        if (options.memory_limit_bytes == 0) {
            return {false, {}, "JavaScript memory limit must be positive"};
        }

        JS_SetMemoryLimit(runtime_.get(), options.memory_limit_bytes);
        Deadline deadline{std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(options.timeout_ms)};
        JS_SetInterruptHandler(runtime_.get(), interrupt_at_deadline, &deadline);
        JSValue value = JS_Eval(context_.get(), script.data(), script.size(),
                                "<prowsetk>", JS_EVAL_TYPE_GLOBAL);
        JS_SetInterruptHandler(runtime_.get(), nullptr, nullptr);

        if (JS_IsException(value)) {
            JS_FreeValue(context_.get(), value);
            return {false, {}, take_exception(context_.get())};
        }
        std::string result = value_to_string(context_.get(), value);
        JS_FreeValue(context_.get(), value);
        if (JS_HasException(context_.get())) {
            return {false, {}, take_exception(context_.get())};
        }
        return {true, std::move(result), {}};
    }

    void set_global(std::string_view name, std::string_view value) override {
        JSValue global = JS_GetGlobalObject(context_.get());
        JSValue string_value = JS_NewStringLen(context_.get(), value.data(), value.size());
        const std::string property(name);
        if (JS_SetPropertyStr(context_.get(), global, property.c_str(), string_value) < 0) {
            (void)take_exception(context_.get());
        }
        JS_FreeValue(context_.get(), global);
    }

    std::string name() const override { return "quickjs"; }

    CapabilitySet capabilities() const override {
        CapabilitySet capabilities;
        capabilities.set("javascript", ImplementationClass::FullyImplemented,
                         "QuickJS page scripting runtime");
        capabilities.set("console", ImplementationClass::Unsupported,
                         "console host binding is not installed yet");
        capabilities.set("fetch", ImplementationClass::Unsupported,
                         "network host binding is not installed yet");
        return capabilities;
    }

private:
    RuntimePtr runtime_;
    ContextPtr context_;
};

}  // namespace

std::unique_ptr<JavaScriptRuntime> make_javascript_runtime() {
    return std::make_unique<QuickJavaScriptRuntime>();
}

}  // namespace prowsetk

#else

namespace prowsetk {

std::unique_ptr<JavaScriptRuntime> make_javascript_runtime() {
    return make_null_javascript_runtime();
}

}  // namespace prowsetk

#endif
