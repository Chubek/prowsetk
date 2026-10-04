#include "flatworm/module_bindings.hpp"

#ifdef PROWSETK_HAVE_QUICKJS

#include <algorithm>
#include <utility>

#include "flatworm/module_internal.hpp"
#include "prowsetk/error.hpp"

namespace prowsetk::flatworm {
namespace {

class Value {
public:
    explicit Value(JSContext* context, JSValue value = JS_UNDEFINED)
        : context_(context), value_(value) {}
    ~Value() { JS_FreeValue(context_, value_); }
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    JSValue get() const { return value_; }
    JSValue release() { return std::exchange(value_, JS_UNDEFINED); }
    void reset(JSValue value) {
        JS_FreeValue(context_, value_);
        value_ = value;
    }

private:
    JSContext* context_;
    JSValue value_;
};

void clear_exception(JSContext* context) noexcept {
    if (JS_HasException(context)) JS_FreeValue(context, JS_GetException(context));
}

JSValue status_exception(JSContext* context, FlatwormStatus status) {
    switch (status) {
        case FLATWORM_STATUS_INVALID_ARGUMENT:
            return JS_ThrowTypeError(context, "Flatworm module arguments or result are invalid");
        case FLATWORM_STATUS_RESOURCE_LIMIT:
            return JS_ThrowRangeError(context, "Flatworm module transfer or call limit exceeded");
        default:
            return JS_ThrowInternalError(context, "Flatworm module callback failed");
    }
}

JSValue lower_value(JSContext* context, const FlatwormValue& value,
                    FlatwormStatus& status) {
    status = FLATWORM_STATUS_OK;
    switch (value.type) {
        case FLATWORM_VALUE_UNDEFINED: return JS_UNDEFINED;
        case FLATWORM_VALUE_NULL: return JS_NULL;
        case FLATWORM_VALUE_BOOLEAN: return JS_NewBool(context, value.boolean != 0);
        case FLATWORM_VALUE_NUMBER: return JS_NewFloat64(context, value.number);
        case FLATWORM_VALUE_STRING:
        case FLATWORM_VALUE_JSON:
            break;
        default:
            status = FLATWORM_STATUS_INVALID_ARGUMENT;
            return JS_EXCEPTION;
    }
    if (value.bytes.size > FLATWORM_MODULE_MAX_VALUE_BYTES) {
        status = FLATWORM_STATUS_RESOURCE_LIMIT;
        return JS_EXCEPTION;
    }
    if (value.bytes.data == nullptr && value.bytes.size != 0) {
        status = FLATWORM_STATUS_INVALID_ARGUMENT;
        return JS_EXCEPTION;
    }
    const char* data = value.bytes.data != nullptr ? value.bytes.data : "";
    JSValue result;
    if (value.type == FLATWORM_VALUE_JSON) {
        // QuickJS requires buf[buf_len] == '\0', whereas ABI buffers are only
        // length-delimited and may end exactly at an allocation boundary.
        const std::string terminated(data, value.bytes.size);
        result = JS_ParseJSON(context, terminated.c_str(), terminated.size(), "<flatworm-result>");
    } else {
        result = JS_NewStringLen(context, data, value.bytes.size);
    }
    if (JS_IsException(result)) {
        // Do not reflect native result bytes or parser excerpts in errors.
        clear_exception(context);
        status = FLATWORM_STATUS_INVALID_ARGUMENT;
    }
    return result;
}

struct CallResult {
    explicit CallResult(JSContext* ctx) : result(ctx), context(ctx) {}
    Value result;
    JSContext* context;
    FlatwormStatus status = FLATWORM_STATUS_OK;
};

FlatwormStatus set_result(FlatwormCall* call, const FlatwormValue* value) noexcept {
    if (call == nullptr || call->host_data == nullptr) return FLATWORM_STATUS_INVALID_ARGUMENT;
    auto& result = *static_cast<CallResult*>(call->host_data);
    if (result.status != FLATWORM_STATUS_OK) return result.status;
    if (value == nullptr) return result.status = FLATWORM_STATUS_INVALID_ARGUMENT;
    try {
        FlatwormStatus status;
        Value lowered(result.context, lower_value(result.context, *value, status));
        if (status != FLATWORM_STATUS_OK) return result.status = status;
        result.result.reset(lowered.release());
        return FLATWORM_STATUS_OK;
    } catch (...) {
        clear_exception(result.context);
        return result.status = FLATWORM_STATUS_ERROR;
    }
}

const FlatwormModuleHostApi kHostApi = {
    FLATWORM_MODULE_ABI_VERSION, sizeof(FlatwormModuleHostApi), set_result};

bool argument_bytes(JSContext* context, JSValueConst value, std::string& bytes,
                    std::size_t& total) {
    std::size_t size = 0;
    const char* text = JS_ToCStringLen(context, &size, value);
    if (text == nullptr) return false;
    struct FreeString {
        JSContext* context;
        void operator()(const char* string) const { JS_FreeCString(context, string); }
    };
    const std::unique_ptr<const char, FreeString> owned(text, FreeString{context});
    if (size > FLATWORM_MODULE_MAX_VALUE_BYTES - total) {
        status_exception(context, FLATWORM_STATUS_RESOURCE_LIMIT);
        return false;
    }
    bytes.assign(text, size);
    total += size;
    return true;
}

}  // namespace

struct ModuleBindings::FunctionBinding {
    Instance* instance;
    FlatwormNativeFunction invoke;
};

struct ModuleBindings::Instance {
    ModuleBindings* owner;
    JSContext* context;
    std::shared_ptr<const FlatwormModule> module;
    void* data = nullptr;
    bool initialize_attempted = false;
    JSValue exports = JS_UNDEFINED;
    std::vector<FunctionBinding> functions;

    ~Instance() {
        if (context != nullptr) JS_FreeValue(context, exports);
        if (initialize_attempted && module->impl_->shutdown != nullptr) {
            try {
                module->impl_->shutdown(data);
            } catch (...) {
                // Native exceptions must not escape destruction/the C ABI.
            }
        }
    }
};

ModuleBindings::ModuleBindings(JSRuntime* runtime, JSContext* context) : context_(context) {
    Value global(context, JS_GetGlobalObject(context));
    Value bridge(context, JS_NewObjectProto(context, JS_NULL));
    Value function(context, JS_NewCClosure(context, lookup, "module", nullptr, 1, 0, this));
    if (JS_IsException(bridge.get()) || JS_IsException(function.get()) ||
        JS_DefinePropertyValueStr(context, bridge.get(), "module",
                                    function.release(), JS_PROP_ENUMERABLE) < 0 ||
        JS_PreventExtensions(context, bridge.get()) < 0 ||
        JS_DefinePropertyValueStr(context, global.get(), "Flatworm", bridge.release(),
                                   JS_PROP_ENUMERABLE) < 0) {
        clear_exception(context);
        throw Error(ErrorCode::JavaScriptError, "Flatworm module bridge installation failed");
    }
    JS_SetModuleLoaderFunc(runtime, normalize, load, this);
}

ModuleBindings::~ModuleBindings() {
    release_values();
    // Shut down in reverse installation order, retaining code through shutdown.
    while (!instances_.empty()) instances_.pop_back();
}

void ModuleBindings::release_values() noexcept {
    for (auto& instance : instances_) {
        if (instance->context != nullptr) {
            JS_FreeValue(instance->context, instance->exports);
            instance->exports = JS_UNDEFINED;
            instance->context = nullptr;
        }
    }
    context_ = nullptr;
}

ModuleBindings::Instance* ModuleBindings::find(std::string_view name) const {
    for (const auto& instance : instances_) {
        if (instance->module->info().name == name) return instance.get();
    }
    return nullptr;
}

void ModuleBindings::install(std::shared_ptr<const FlatwormModule> module) {
    if (module == nullptr) throw Error(ErrorCode::InvalidArgument, "module is null");
    if (find(module->info().name) != nullptr) {
        throw Error(ErrorCode::InvalidArgument, "module is already installed");
    }
    if (instances_.size() == FLATWORM_MODULE_MAX_MODULES) {
        throw Error(ErrorCode::ResourceLimit, "module count limit exceeded");
    }
    auto instance = std::make_unique<Instance>();
    instance->owner = this;
    instance->context = context_;
    instance->module = std::move(module);
    instance->exports = JS_NewObjectProto(context_, JS_NULL);
    const auto& definition = *instance->module->impl_;
    instance->functions.reserve(definition.functions.size());
    bool valid = !JS_IsException(instance->exports);
    for (const auto& function : definition.functions) {
        if (!valid) break;
        instance->functions.push_back({instance.get(), function.invoke});
        Value callable(context_, JS_NewCClosure(context_, invoke, function.name.c_str(), nullptr,
            static_cast<int>(function.arity), 0, &instance->functions.back()));
        valid = !JS_IsException(callable.get()) &&
            JS_DefinePropertyValueStr(context_, instance->exports, function.name.c_str(),
                                       callable.release(), JS_PROP_ENUMERABLE) >= 0;
    }
    for (const auto& constant : definition.constants) {
        if (!valid) break;
        auto value = constant.value;
        value.bytes.data = constant.bytes.data();
        FlatwormStatus status;
        Value lowered(context_, lower_value(context_, value, status));
        valid = status == FLATWORM_STATUS_OK && !JS_IsException(lowered.get()) &&
            JS_DefinePropertyValueStr(context_, instance->exports, constant.name.c_str(),
                                       lowered.release(), JS_PROP_ENUMERABLE) >= 0;
    }
    if (!valid || JS_PreventExtensions(context_, instance->exports) < 0) {
        clear_exception(context_);
        throw Error(ErrorCode::JavaScriptError, "Flatworm module exports are invalid or exceed memory limits");
    }
    if (definition.initialize != nullptr) {
        instance->initialize_attempted = true;
        FlatwormStatus status = FLATWORM_STATUS_ERROR;
        try {
            status = definition.initialize(&kHostApi, &instance->data);
        } catch (...) {
            // No native exception text (which may contain secrets) is emitted.
        }
        if (status != FLATWORM_STATUS_OK) {
            throw Error(ErrorCode::JavaScriptError, "Flatworm module initialization failed");
        }
    }
    // Publish only after export construction and initialization succeed.
    instances_.push_back(std::move(instance));
}

std::vector<FlatwormModuleInfo> ModuleBindings::modules() const {
    std::vector<FlatwormModuleInfo> result;
    for (const auto& instance : instances_) result.push_back(instance->module->info());
    return result;
}

JSValue ModuleBindings::lookup(JSContext* context, JSValueConst, int argc,
                                JSValueConst* argv, int, void* opaque) {
    try {
        if (argc != 1 || !JS_IsString(argv[0])) {
            return JS_ThrowTypeError(context, "Flatworm.module requires one module name");
        }
        std::string name;
        std::size_t size = 0;
        if (!argument_bytes(context, argv[0], name, size)) return JS_EXCEPTION;
        auto* instance = static_cast<ModuleBindings*>(opaque)->find(name);
        if (instance == nullptr) return JS_ThrowReferenceError(context, "Flatworm module is not installed");
        return JS_DupValue(context, instance->exports);
    } catch (...) {
        clear_exception(context);
        return JS_ThrowInternalError(context, "Flatworm module lookup failed");
    }
}

JSValue ModuleBindings::invoke(JSContext* context, JSValueConst, int argc,
                                JSValueConst* argv, int, void* opaque) {
    try {
        auto& binding = *static_cast<FunctionBinding*>(opaque);
        auto& owner = *binding.instance->owner;
        if (argc < 0 || static_cast<unsigned>(argc) > FLATWORM_MODULE_MAX_ARGUMENTS ||
            owner.call_depth_ == FLATWORM_MODULE_MAX_CALL_DEPTH) {
            return status_exception(context, FLATWORM_STATUS_RESOURCE_LIMIT);
        }
        struct DepthScope {
            explicit DepthScope(std::size_t& value) : depth(value) { ++depth; }
            ~DepthScope() { --depth; }
            std::size_t& depth;
        } depth_scope(owner.call_depth_);
        const auto count = static_cast<std::size_t>(argc);
        std::vector<FlatwormValue> arguments(count);
        std::vector<std::string> strings(count);
        std::size_t byte_count = 0;
        for (std::size_t i = 0; i < count; ++i) {
            auto& argument = arguments[i];
            const auto value = argv[i];
            if (JS_IsUndefined(value)) argument.type = FLATWORM_VALUE_UNDEFINED;
            else if (JS_IsNull(value)) argument.type = FLATWORM_VALUE_NULL;
            else if (JS_IsBool(value)) {
                argument.type = FLATWORM_VALUE_BOOLEAN;
                argument.boolean = JS_ToBool(context, value);
            } else if (JS_IsNumber(value)) {
                argument.type = FLATWORM_VALUE_NUMBER;
                if (JS_ToFloat64(context, &argument.number, value) < 0) return JS_EXCEPTION;
            } else if (JS_IsString(value)) {
                argument.type = FLATWORM_VALUE_STRING;
                if (!argument_bytes(context, value, strings[i], byte_count)) return JS_EXCEPTION;
            } else if (JS_IsObject(value) && !JS_IsFunction(context, value)) {
                argument.type = FLATWORM_VALUE_JSON;
                Value json(context, JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED));
                if (JS_IsException(json.get())) {
                    // Getters/toJSON can throw messages containing argument
                    // values. Keep conversion failures as value-free ABI errors.
                    clear_exception(context);
                    return status_exception(context, FLATWORM_STATUS_INVALID_ARGUMENT);
                }
                if (JS_IsUndefined(json.get())) {
                    return status_exception(context, FLATWORM_STATUS_INVALID_ARGUMENT);
                }
                if (!argument_bytes(context, json.get(), strings[i], byte_count)) return JS_EXCEPTION;
            } else {
                return status_exception(context, FLATWORM_STATUS_INVALID_ARGUMENT);
            }
            argument.bytes = {strings[i].data(), strings[i].size()};
        }
        CallResult result(context);
        FlatwormCall call{&kHostApi, binding.instance->data, arguments.data(), count, &result};
        FlatwormStatus status = FLATWORM_STATUS_ERROR;
        try {
            status = binding.invoke(&call);
        } catch (...) {
            // The C ABI must not unwind through QuickJS.
        }
        if (result.status != FLATWORM_STATUS_OK) status = result.status;
        if (status != FLATWORM_STATUS_OK) return status_exception(context, status);
        return result.result.release();
    } catch (...) {
        clear_exception(context);
        return JS_ThrowInternalError(context, "Flatworm module invocation failed");
    }
}

char* ModuleBindings::normalize(JSContext* context, const char*, const char* name, void* opaque) {
    constexpr std::string_view prefix = "flatworm:";
    const std::string_view specifier(name);
    if (!specifier.starts_with(prefix) ||
        static_cast<ModuleBindings*>(opaque)->find(specifier.substr(prefix.size())) == nullptr) {
        JS_ThrowReferenceError(context, "only installed flatworm: modules may be imported");
        return nullptr;
    }
    return js_strdup(context, name);
}

JSModuleDef* ModuleBindings::load(JSContext* context, const char* name, void* opaque) {
    auto* instance = static_cast<ModuleBindings*>(opaque)->find(std::string_view(name).substr(9));
    if (instance == nullptr) {
        JS_ThrowReferenceError(context, "Flatworm module is not installed");
        return nullptr;
    }
    auto* module = JS_NewCModule(context, name, initialize_exports);
    if (module == nullptr) return nullptr;
    JS_SetModulePrivateValue(context, module, JS_DupValue(context, instance->exports));
    for (const auto& export_name : instance->module->info().exports) {
        if (JS_AddModuleExport(context, module, export_name.c_str()) < 0) return nullptr;
    }
    return module;
}

int ModuleBindings::initialize_exports(JSContext* context, JSModuleDef* module) {
    Value exports(context, JS_GetModulePrivateValue(context, module));
    JSPropertyEnum* properties = nullptr;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(context, &properties, &count, exports.get(),
                                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return -1;
    struct FreeProperties {
        JSContext* context;
        uint32_t count;
        void operator()(JSPropertyEnum* table) const { JS_FreePropertyEnum(context, table, count); }
    };
    const std::unique_ptr<JSPropertyEnum, FreeProperties> owned(properties, FreeProperties{context, count});
    for (uint32_t i = 0; i < count; ++i) {
        const char* name = JS_AtomToCString(context, properties[i].atom);
        if (name == nullptr) return -1;
        Value value(context, JS_GetProperty(context, exports.get(), properties[i].atom));
        const int status = JS_IsException(value.get()) ? -1
            : JS_SetModuleExport(context, module, name, value.release());
        JS_FreeCString(context, name);
        if (status < 0) return -1;
    }
    return 0;
}

}  // namespace prowsetk::flatworm

#endif
