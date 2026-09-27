#ifndef PROWSETK_FLATWORM_SELECTOR_MODEL_HPP
#define PROWSETK_FLATWORM_SELECTOR_MODEL_HPP

#include <string>
#include <string_view>
#include <vector>

namespace prowsetk::flatworm {
struct Node;
namespace selectors {
// Syntax data only: parsing does not access the DOM; matching does not parse.
enum class Combinator { Descendant, Child, Adjacent, Sibling };

enum class AttrOp { Exists, Equals, Includes, DashMatch, Prefix, Suffix, Substring };

struct AttrSelector {
    std::string name;
    AttrOp op = AttrOp::Exists;
    std::string value;
    bool ascii_insensitive = false;
};

enum class PseudoKind {
    FirstChild,
    LastChild,
    OnlyChild,
    NthChild,
    NthLastChild,
    FirstOfType,
    LastOfType,
    OnlyOfType,
    NthOfType,
    NthLastOfType,
    Empty,
    Root,
    Not
};

struct PseudoClass {
    PseudoKind kind = PseudoKind::FirstChild;
    int nth_a = 0;
    int nth_b = 0;
    std::vector<struct ComplexSelector> not_list;
};

struct Compound {
    bool universal = false;
    std::string tag;
    std::vector<std::string> ids;
    std::vector<std::string> classes;
    std::vector<AttrSelector> attributes;
    std::vector<PseudoClass> pseudos;
};

struct ComplexSelector {
    std::vector<Compound> compounds;
    std::vector<Combinator> combinators;
};


inline bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
inline std::string to_lower(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return value;
}
std::vector<ComplexSelector> parse(std::string_view text);
bool matches(const Node& node, const std::vector<ComplexSelector>& selectors);
}  // namespace selectors
}  // namespace prowsetk::flatworm
#endif
