// ir_registry.cpp — plugin-extensibility stratum.
//
// Named IR emitters beyond the four built-ins. Native plugins register text
// or binary emitters here; the `lprowseir.emit` / `lprowseir.emitters` Lua
// pipeline resolves through the same registry, so drivers and plugins agree
// on IR names. The registry never walks documents itself — it only dispatches
// to the emitter that owns the format.

#include "prowsetk/ir.hpp"

#include <algorithm>

namespace prowsetk {

IrEmitterRegistry::IrEmitterRegistry() {
    text_.emplace("iml", [](const Document& document) {
        return emit_prowse_iml(document);
    });
    binary_.emplace("vtd", [](const Document& document) {
        return emit_prowse_vtd(document);
    });
}

IrEmitterRegistry& IrEmitterRegistry::global() {
    static IrEmitterRegistry instance;
    return instance;
}

bool IrEmitterRegistry::register_text(std::string_view name,
                                      IrTextEmitter emitter) {
    if (name.empty() || !emitter) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    text_[std::string(name)] = std::move(emitter);
    return true;
}

bool IrEmitterRegistry::register_binary(std::string_view name,
                                        IrBinaryEmitter emitter) {
    if (name.empty() || !emitter) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    binary_[std::string(name)] = std::move(emitter);
    return true;
}

bool IrEmitterRegistry::unregister(std::string_view name) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string key(name);
    return text_.erase(key) + binary_.erase(key) > 0;
}

void IrEmitterRegistry::clear_custom() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = text_.begin(); it != text_.end();) {
        if (it->first != "iml") {
            it = text_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = binary_.begin(); it != binary_.end();) {
        if (it->first != "vtd") {
            it = binary_.erase(it);
        } else {
            ++it;
        }
    }
}

bool IrEmitterRegistry::contains(std::string_view name) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string key(name);
    return text_.find(key) != text_.end() ||
           binary_.find(key) != binary_.end();
}

std::vector<std::string> IrEmitterRegistry::names() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(text_.size() + binary_.size());
    for (const auto& [name, _] : text_) {
        out.push_back(name);
    }
    for (const auto& [name, _] : binary_) {
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::string> IrEmitterRegistry::emit_text(
    const Document& document, std::string_view name) const {
    IrTextEmitter emitter;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = text_.find(std::string(name));
        if (found == text_.end()) {
            return std::nullopt;
        }
        emitter = found->second;
    }
    return emitter(document);
}

std::optional<std::vector<std::uint8_t>> IrEmitterRegistry::emit_binary(
    const Document& document, std::string_view name) const {
    IrBinaryEmitter emitter;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = binary_.find(std::string(name));
        if (found == binary_.end()) {
            return std::nullopt;
        }
        emitter = found->second;
    }
    return emitter(document);
}

}  // namespace prowsetk
