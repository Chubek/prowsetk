#ifndef PROWSETK_CAPABILITY_HPP
#define PROWSETK_CAPABILITY_HPP

#include <string>
#include <string_view>
#include <vector>

namespace prowsetk {

// Compatibility classification carried by every supported feature, as required
// by the compatibility policy in README.md.
enum class ImplementationClass {
    FullyImplemented,
    PartiallyImplemented,
    ImplementedWithRestrictions,
    DummyImplementation,
    Unsupported
};

const char* to_string(ImplementationClass classification) noexcept;

struct Capability {
    std::string name;
    ImplementationClass classification = ImplementationClass::Unsupported;
    std::string notes;
};

// A queryable collection of capability declarations. `has` reports only
// capabilities that are actually usable (anything above Unsupported).
class CapabilitySet {
public:
    void add(Capability capability);
    void set(std::string name, ImplementationClass classification,
             std::string notes = {});

    bool has(std::string_view name) const noexcept;

    const Capability* find(std::string_view name) const noexcept;

    ImplementationClass classification(std::string_view name) const noexcept;

    std::vector<Capability> all() const;

private:
    std::vector<Capability> capabilities_;
};

}  // namespace prowsetk

#endif  // PROWSETK_CAPABILITY_HPP
