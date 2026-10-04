#ifndef PROWSETK_FLATWORM_MODULE_BINDINGS_HPP
#define PROWSETK_FLATWORM_MODULE_BINDINGS_HPP

#ifdef PROWSETK_HAVE_QUICKJS
#include <quickjs.h>

#include "prowsetk/flatworm_module.hpp"

namespace prowsetk::flatworm {

// QuickJS lowering of the backend-independent Flatwork-Module.h contract.
// Owned by one runtime; no browser plugins, Lua, network or DOM dependencies.
class ModuleBindings {
public:
    ModuleBindings(JSRuntime* runtime, JSContext* context);
    ~ModuleBindings();
    void install(std::shared_ptr<const FlatwormModule> module);
    std::vector<FlatwormModuleInfo> modules() const;

    // Drop our JS references before destroying the context/runtime. Native
    // instances and libraries stay alive until this object is then destroyed.
    void release_values() noexcept;

private:
    struct Instance;
    struct FunctionBinding;
    std::vector<std::unique_ptr<Instance>> instances_;
    JSContext* context_;
    std::size_t call_depth_ = 0;

    Instance* find(std::string_view name) const;
    static JSValue lookup(JSContext*, JSValueConst, int, JSValueConst*, int, void*);
    static JSValue invoke(JSContext*, JSValueConst, int, JSValueConst*, int, void*);
    static char* normalize(JSContext*, const char*, const char*, void*);
    static JSModuleDef* load(JSContext*, const char*, void*);
    static int initialize_exports(JSContext*, JSModuleDef*);
};

}  // namespace prowsetk::flatworm
#endif

#endif  // PROWSETK_FLATWORM_MODULE_BINDINGS_HPP
