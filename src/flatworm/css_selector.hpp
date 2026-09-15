#ifndef PROWSETK_FLATWORM_CSS_SELECTOR_HPP
#define PROWSETK_FLATWORM_CSS_SELECTOR_HPP

#include <memory>
#include <string_view>
#include <vector>

#include "flatworm/dom_internal.hpp"

namespace prowsetk::flatworm {

// A parsed CSS selector list. Parsing throws prowsetk::Error(ParseError) when
// the selector is malformed.
class Selector {
public:
    Selector();
    ~Selector();
    Selector(Selector&&) noexcept;
    Selector& operator=(Selector&&) noexcept;
    Selector(const Selector&) = delete;
    Selector& operator=(const Selector&) = delete;

    static Selector parse(std::string_view text);

    bool matches(const Node& node) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::shared_ptr<Node> query_selector(const std::shared_ptr<Node>& root,
                                     std::string_view selector);
std::vector<std::shared_ptr<Node>> query_selector_all(
    const std::shared_ptr<Node>& root, std::string_view selector);

}  // namespace prowsetk::flatworm

#endif  // PROWSETK_FLATWORM_CSS_SELECTOR_HPP
