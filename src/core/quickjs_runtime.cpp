#include "prowsetk/javascript_runtime.hpp"

#ifdef PROWSETK_HAVE_QUICKJS

#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <quickjs.h>

namespace prowsetk {
namespace {

class QuickJavaScriptRuntime;

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
    bool expired = false;
};

int interrupt_at_deadline(JSRuntime*, void* opaque) {
    auto* deadline = static_cast<Deadline*>(opaque);
    deadline->expired = deadline->expired ||
                        std::chrono::steady_clock::now() >= deadline->expires_at;
    return deadline->expired ? 1 : 0;
}

class ExecutionScope {
public:
    ExecutionScope(JSRuntime* runtime, bool& active, const ScriptOptions& options)
        : runtime_(runtime), active_(active),
          deadline_{std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(options.timeout_ms)} {
        active_ = true;
        JS_UpdateStackTop(runtime_);
        JS_SetMemoryLimit(runtime_, options.memory_limit_bytes);
        JS_SetInterruptHandler(runtime_, interrupt_at_deadline, &deadline_);
    }

    ~ExecutionScope() {
        JS_SetInterruptHandler(runtime_, nullptr, nullptr);
        JS_SetMemoryLimit(runtime_, std::numeric_limits<std::size_t>::max());
        active_ = false;
    }

    bool expired() { return interrupt_at_deadline(runtime_, &deadline_) != 0; }

private:
    JSRuntime* runtime_;
    bool& active_;
    Deadline deadline_;
};

std::string validate_options(const ScriptOptions& options) {
    if (options.timeout_ms <= 0) return "JavaScript timeout must be positive";
    if (options.memory_limit_bytes == 0) return "JavaScript memory limit must be positive";
    if (options.max_microtask_jobs == 0) return "JavaScript microtask limit must be positive";
    return {};
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
    if (JS_HasException(context)) {
        JS_FreeValue(context, JS_GetException(context));
    }
    return message.empty() ? "JavaScript evaluation failed" : message;
}

// Defined after QuickJavaScriptRuntime so the console bindings can reach its
// console handler through the context opaque slot.
int install_console_binding(JSContext* context);

JSValue microtask_job(JSContext* context, int, JSValueConst* argv) {
    return JS_Call(context, argv[0], JS_UNDEFINED, 0, nullptr);
}

JSValue queue_microtask(JSContext* context, JSValueConst, int argc,
                        JSValueConst* argv) {
    if (argc == 0 || !JS_IsFunction(context, argv[0])) {
        return JS_ThrowTypeError(context, "queueMicrotask requires a callable");
    }
    if (JS_EnqueueJob(context, microtask_job, 1, argv) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

class QuickJavaScriptRuntime final : public JavaScriptRuntime {
public:
    QuickJavaScriptRuntime()
        : runtime_(JS_NewRuntime()),
          context_(runtime_ != nullptr ? JS_NewContext(runtime_.get()) : nullptr) {
        if (runtime_ == nullptr || context_ == nullptr) throw std::bad_alloc();
        JS_SetCanBlock(runtime_.get(), false);
        JS_SetContextOpaque(context_.get(), this);
        if (install_console_binding(context_.get()) < 0) throw std::bad_alloc();
        JSValue global = JS_GetGlobalObject(context_.get());
        const int status = JS_SetPropertyStr(
            context_.get(), global, "queueMicrotask",
            JS_NewCFunction(context_.get(), queue_microtask, "queueMicrotask", 1));
        JS_FreeValue(context_.get(), global);
        if (status < 0) throw std::bad_alloc();
    }

    ~QuickJavaScriptRuntime() override {
        if (context_ != nullptr) {
            JS_SetContextOpaque(context_.get(), nullptr);
        }
    }

    void forward_console(ConsoleMessage message) {
        if (console_handler_) {
            try {
                auto handler = console_handler_;
                handler(message);
            } catch (...) {
                // A console handler must not break page evaluation.
            }
        }
    }

    ScriptResult evaluate(std::string_view script,
                          const ScriptOptions& options) override {
        if (active_) return {false, {}, "JavaScript runtime is already executing"};
        if (const auto error = validate_options(options); !error.empty()) {
            return {false, {}, error};
        }
        const std::string source(script);
        ExecutionScope scope(runtime_.get(), active_, options);
        JSValue value = JS_Eval(context_.get(), source.c_str(), source.size(),
                                "<prowsetk>", JS_EVAL_TYPE_GLOBAL);
        ScriptResult result;
        if (JS_IsException(value)) {
            result.error = take_exception(context_.get());
        } else {
            result.value = value_to_string(context_.get(), value);
            if (JS_HasException(context_.get())) {
                result.value.clear();
                result.error = take_exception(context_.get());
            } else {
                result.ok = true;
            }
        }
        JS_FreeValue(context_.get(), value);
        if (scope.expired()) return {false, {}, "JavaScript execution deadline exceeded"};
        const auto checkpoint = drain_microtasks(options, scope);
        if (!checkpoint.ok && result.ok) return checkpoint;
        return result;
    }

    ScriptResult run_microtasks(const ScriptOptions& options) override {
        if (active_) return {false, {}, "JavaScript runtime is already executing"};
        if (const auto error = validate_options(options); !error.empty()) {
            return {false, {}, error};
        }
        ExecutionScope scope(runtime_.get(), active_, options);
        return drain_microtasks(options, scope);
    }

    bool has_pending_microtasks() const override {
        return JS_IsJobPending(runtime_.get());
    }

    void set_global(std::string_view name, std::string_view value) override {
        if (active_) return;
        ExecutionScope scope(runtime_.get(), active_, ScriptOptions{});
        JSValue global = JS_GetGlobalObject(context_.get());
        const JSAtom property = JS_NewAtomLen(context_.get(), name.data(), name.size());
        if (property != JS_ATOM_NULL) {
            JSValue string_value = JS_NewStringLen(context_.get(), value.data(), value.size());
            if (!JS_IsException(string_value)) {
                JS_SetProperty(context_.get(), global, property, string_value);
            }
            JS_FreeAtom(context_.get(), property);
        }
        if (JS_HasException(context_.get())) (void)take_exception(context_.get());
        JS_FreeValue(context_.get(), global);
    }

    void set_console_handler(ConsoleHandler handler) override {
        console_handler_ = std::move(handler);
    }

    std::string name() const override { return "quickjs"; }

    CapabilitySet capabilities() const override {
        CapabilitySet capabilities;
        capabilities.set("javascript", ImplementationClass::FullyImplemented,
                         "QuickJS page scripting runtime");
        capabilities.set("console", ImplementationClass::PartiallyImplemented,
                          "log/info/warn/error/debug; space-joined string conversion, no format substitutions");
        capabilities.set("promises", ImplementationClass::ImplementedWithRestrictions,
                          "ECMAScript promises; bounded checkpoints after evaluation, no implicit promise unwrapping or rejection events");
        capabilities.set("queueMicrotask", ImplementationClass::ImplementedWithRestrictions,
                          "FIFO jobs; bounded checkpoints, remaining jobs retained on limit or callback failure");
        capabilities.set("fetch", ImplementationClass::Unsupported,
                         "network host binding is not installed yet");
        return capabilities;
    }

private:
    ScriptResult drain_microtasks(const ScriptOptions& options, ExecutionScope& scope) {
        std::size_t executed = 0;
        while (JS_IsJobPending(runtime_.get())) {
            if (scope.expired()) return {false, {}, "JavaScript execution deadline exceeded"};
            if (executed == options.max_microtask_jobs) {
                return {false, {}, "JavaScript microtask job limit exceeded"};
            }
            JSContext* job_context = nullptr;
            const int status = JS_ExecutePendingJob(runtime_.get(), &job_context);
            ++executed;
            if (status < 0) {
                return {false, {}, take_exception(job_context != nullptr ? job_context : context_.get())};
            }
        }
        if (scope.expired()) return {false, {}, "JavaScript execution deadline exceeded"};
        return {true, "undefined", {}};
    }

    RuntimePtr runtime_;
    ContextPtr context_;
    ConsoleHandler console_handler_;
    bool active_ = false;
};

// Called by the `console.log/info/warn/error/debug` host bindings. Reads the
// runtime pointer from the context opaque slot and forwards to its handler.
JSValue console_method(JSContext* context, const char* level, int argc,
                       JSValueConst* argv) {
    auto* runtime =
        static_cast<QuickJavaScriptRuntime*>(JS_GetContextOpaque(context));
    if (runtime == nullptr) {
        return JS_UNDEFINED;
    }
    ConsoleMessage message;
    message.level = level;
    for (int i = 0; i < argc; ++i) {
        if (i != 0) message.text += ' ';
        message.text += value_to_string(context, argv[i]);
        if (JS_HasException(context)) return JS_EXCEPTION;
    }
    runtime->forward_console(std::move(message));
    return JS_UNDEFINED;
}

JSValue console_log(JSContext* context, JSValueConst, int argc,
                    JSValueConst* argv) {
    return console_method(context, "log", argc, argv);
}
JSValue console_info(JSContext* context, JSValueConst, int argc,
                     JSValueConst* argv) {
    return console_method(context, "info", argc, argv);
}
JSValue console_warn(JSContext* context, JSValueConst, int argc,
                     JSValueConst* argv) {
    return console_method(context, "warn", argc, argv);
}
JSValue console_error(JSContext* context, JSValueConst, int argc,
                      JSValueConst* argv) {
    return console_method(context, "error", argc, argv);
}
JSValue console_debug(JSContext* context, JSValueConst, int argc,
                      JSValueConst* argv) {
    return console_method(context, "debug", argc, argv);
}

int install_console_binding(JSContext* context) {
    JSValue global = JS_GetGlobalObject(context);
    JSValue console = JS_NewObject(context);
    JS_SetPropertyStr(context, console, "log",
                      JS_NewCFunction(context, console_log, "log", 1));
    JS_SetPropertyStr(context, console, "info",
                      JS_NewCFunction(context, console_info, "info", 1));
    JS_SetPropertyStr(context, console, "warn",
                      JS_NewCFunction(context, console_warn, "warn", 1));
    JS_SetPropertyStr(context, console, "error",
                      JS_NewCFunction(context, console_error, "error", 1));
    JS_SetPropertyStr(context, console, "debug",
                      JS_NewCFunction(context, console_debug, "debug", 1));
    const int rc = JS_SetPropertyStr(context, global, "console", console);
    // JS_SetPropertyStr transfers ownership of `console`; do not free it here.
    JS_FreeValue(context, global);
    return rc;
}

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
