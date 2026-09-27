#include "flatworm/css_selector.hpp"
#include "flatworm/selector_model.hpp"

namespace prowsetk::flatworm {
struct Selector::Impl {
    std::vector<selectors::ComplexSelector> selectors;
};

Selector::Selector() = default;
Selector::~Selector() = default;
Selector::Selector(Selector&&) noexcept = default;
Selector& Selector::operator=(Selector&&) noexcept = default;

Selector Selector::parse(std::string_view text) {
    Selector selector;
    selector.impl_ = std::make_unique<Impl>();
    selector.impl_->selectors = selectors::parse(text);
    return selector;
}

bool Selector::matches(const Node& node) const {
    if (impl_ == nullptr) {
        return false;
    }
    return selectors::matches(node, impl_->selectors);
}

namespace {
// Explicit preorder stack avoids recursion proportional to document depth.
std::vector<std::shared_ptr<Node>> collect(const std::shared_ptr<Node>& root,
                                         const Selector& selector,
                                         bool first_only, bool include_root) {
    std::vector<std::shared_ptr<Node>> result;
    std::vector<std::shared_ptr<Node>> pending;
    if (root) pending.push_back(root);
    while (!pending.empty()) {
        auto node = std::move(pending.back());
        pending.pop_back();
        if ((include_root || node != root) && selector.matches(*node)) {
            result.push_back(node);
            if (first_only) break;
        }
        pending.insert(pending.end(), node->children.rbegin(), node->children.rend());
    }
    return result;
}
}  // namespace

std::shared_ptr<Node> query_selector(const std::shared_ptr<Node>& root,
                                     std::string_view text, bool include_root) {
    auto result = collect(root, Selector::parse(text), true, include_root);
    return result.empty() ? nullptr : result.front();
}

std::vector<std::shared_ptr<Node>> query_selector_all(
    const std::shared_ptr<Node>& root, std::string_view text, bool include_root) {
    return collect(root, Selector::parse(text), false, include_root);
}
}  // namespace prowsetk::flatworm
