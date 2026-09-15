#include "prowsetk/capability.hpp"

#include <algorithm>

namespace prowsetk {

const char* to_string(ImplementationClass classification) noexcept {
    switch (classification) {
        case ImplementationClass::FullyImplemented:
            return "fully-implemented";
        case ImplementationClass::PartiallyImplemented:
            return "partially-implemented";
        case ImplementationClass::ImplementedWithRestrictions:
            return "implemented-with-restrictions";
        case ImplementationClass::DummyImplementation:
            return "dummy-implementation";
        case ImplementationClass::Unsupported:
            return "unsupported";
    }
    return "unsupported";
}

void CapabilitySet::add(Capability capability) {
    set(std::move(capability.name), capability.classification,
        std::move(capability.notes));
}

void CapabilitySet::set(std::string name, ImplementationClass classification,
                        std::string notes) {
    for (auto& existing : capabilities_) {
        if (existing.name == name) {
            existing.classification = classification;
            existing.notes = std::move(notes);
            return;
        }
    }
    capabilities_.push_back(
        Capability{std::move(name), classification, std::move(notes)});
}

bool CapabilitySet::has(std::string_view name) const noexcept {
    const Capability* capability = find(name);
    return capability != nullptr &&
           capability->classification != ImplementationClass::Unsupported;
}

const Capability* CapabilitySet::find(std::string_view name) const noexcept {
    for (const auto& capability : capabilities_) {
        if (capability.name == name) {
            return &capability;
        }
    }
    return nullptr;
}

ImplementationClass CapabilitySet::classification(
    std::string_view name) const noexcept {
    const Capability* capability = find(name);
    return capability != nullptr ? capability->classification
                                 : ImplementationClass::Unsupported;
}

std::vector<Capability> CapabilitySet::all() const { return capabilities_; }

}  // namespace prowsetk
