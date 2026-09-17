#include "flatworm/css_selector.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>

#include "prowsetk/error.hpp"

namespace prowsetk::flatworm {
namespace {

enum class Combinator { Descendant, Child, Adjacent, Sibling };

enum class AttrOp { Exists, Equals, Includes, DashMatch, Prefix, Suffix, Substring };

struct AttrSelector {
    std::string name;
    AttrOp op = AttrOp::Exists;
    std::string value;
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
    std::vector<struct Compound> not_list;
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

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ||
           c == '_' || c == '\\' ||
           static_cast<unsigned char>(c) >= 0x80;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

std::string read_ident(std::string_view text, std::size_t& i) {
    std::string result;
    while (i < text.size() && is_ident_char(text[i])) {
        if (text[i] == '\\') {
            if (i + 1 == text.size() || text[i + 1] == '\n' ||
                text[i + 1] == '\r' || text[i + 1] == '\f') {
                throw Error(ErrorCode::ParseError, "invalid selector escape");
            }
            result.push_back(text[i + 1]);
            i += 2;
            continue;
        }
        result.push_back(text[i++]);
    }
    return result;
}

std::vector<std::string> split_whitespace(std::string_view value) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : value) {
        if (is_space(c)) {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::size_t element_index(const Node& node, bool of_type = false) {
    const auto parent = node.shared_parent();
    if (parent == nullptr) {
        return 1;
    }
    std::size_t index = 0;
    for (const auto& sibling : parent->children) {
        if (sibling->is_element() && (!of_type || sibling->name == node.name)) {
            ++index;
        }
        if (sibling.get() == &node) {
            return index;
        }
    }
    return index;
}

std::size_t element_count(const Node& node, bool of_type = false) {
    const auto parent = node.shared_parent();
    if (parent == nullptr) {
        return 1;
    }
    return static_cast<std::size_t>(
        std::count_if(parent->children.begin(), parent->children.end(),
                      [&](const std::shared_ptr<Node>& child) {
                          return child->is_element() &&
                                 (!of_type || child->name == node.name);
                      }));
}

bool match_compound(const Node& node, const Compound& compound);

bool match_pseudo(const Node& node, const PseudoClass& pseudo) {
    switch (pseudo.kind) {
        case PseudoKind::FirstChild:
            return element_index(node) == 1;
        case PseudoKind::LastChild:
            return element_index(node) == element_count(node);
        case PseudoKind::OnlyChild:
            return element_count(node) == 1;
        case PseudoKind::FirstOfType:
            return element_index(node, true) == 1;
        case PseudoKind::LastOfType:
            return element_index(node, true) == element_count(node, true);
        case PseudoKind::OnlyOfType:
            return element_count(node, true) == 1;
        case PseudoKind::Empty: {
            for (const auto& child : node.children) {
                if (child->is_element() || child->is_text()) {
                    if (child->is_text() && child->text.empty()) {
                        continue;
                    }
                    return false;
                }
            }
            return true;
        }
        case PseudoKind::Root:
            return node.shared_parent() != nullptr &&
                   node.shared_parent()->type == NodeType::Document;
        case PseudoKind::NthChild:
        case PseudoKind::NthLastChild:
        case PseudoKind::NthOfType:
        case PseudoKind::NthLastOfType: {
            const bool of_type = pseudo.kind == PseudoKind::NthOfType ||
                                 pseudo.kind == PseudoKind::NthLastOfType;
            const bool reverse = pseudo.kind == PseudoKind::NthLastChild ||
                                 pseudo.kind == PseudoKind::NthLastOfType;
            const auto position = element_index(node, of_type);
             const auto index = static_cast<std::int64_t>(reverse
                ? element_count(node, of_type) - position + 1 : position);
            const std::int64_t a = pseudo.nth_a;
            const std::int64_t b = pseudo.nth_b;
            if (a == 0) {
                return index == b;
            }
            const std::int64_t delta = index - b;
            return delta % a == 0 && delta / a >= 0;
        }
        case PseudoKind::Not: {
            for (const auto& compound : pseudo.not_list) {
                if (match_compound(node, compound)) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

bool match_attr(const Node& node, const AttrSelector& attr) {
    const std::string* value = node.attribute(attr.name);
    if (value == nullptr) {
        return false;
    }
    switch (attr.op) {
        case AttrOp::Exists:
            return true;
        case AttrOp::Equals:
            return *value == attr.value;
        case AttrOp::Includes:
            for (const auto& part : split_whitespace(*value)) {
                if (part == attr.value) {
                    return true;
                }
            }
            return false;
        case AttrOp::DashMatch:
            return *value == attr.value ||
                   (value->size() > attr.value.size() &&
                    value->compare(0, attr.value.size(), attr.value) == 0 &&
                    (*value)[attr.value.size()] == '-');
        case AttrOp::Prefix:
            return !attr.value.empty() && value->rfind(attr.value, 0) == 0;
        case AttrOp::Suffix:
            return !attr.value.empty() && value->size() >= attr.value.size() &&
                   value->compare(value->size() - attr.value.size(),
                                  attr.value.size(), attr.value) == 0;
        case AttrOp::Substring:
            return !attr.value.empty() &&
                   value->find(attr.value) != std::string::npos;
    }
    return false;
}

bool match_compound(const Node& node, const Compound& compound) {
    if (!node.is_element()) {
        return false;
    }
    if (!compound.universal && !compound.tag.empty() &&
        node.name != compound.tag) {
        return false;
    }
    for (const auto& wanted : compound.ids) {
        const std::string* id = node.attribute("id");
        if (id == nullptr || *id != wanted) {
            return false;
        }
    }
    if (!compound.classes.empty()) {
        const std::string* class_attr = node.attribute("class");
        if (class_attr == nullptr) {
            return false;
        }
        const auto classes = split_whitespace(*class_attr);
        for (const auto& wanted : compound.classes) {
            if (std::find(classes.begin(), classes.end(), wanted) ==
                classes.end()) {
                return false;
            }
        }
    }
    for (const auto& attr : compound.attributes) {
        if (!match_attr(node, attr)) {
            return false;
        }
    }
    for (const auto& pseudo : compound.pseudos) {
        if (!match_pseudo(node, pseudo)) {
            return false;
        }
    }
    return true;
}

bool match_complex(const Node& node, const ComplexSelector& selector,
                   std::size_t index) {
    if (!match_compound(node, selector.compounds[index])) {
        return false;
    }
    if (index == 0) {
        return true;
    }
    const Combinator combinator = selector.combinators[index - 1];
    const Node* current = &node;
    switch (combinator) {
        case Combinator::Descendant: {
            auto parent = current->shared_parent();
            while (parent != nullptr) {
                if (match_complex(*parent, selector, index - 1)) {
                    return true;
                }
                parent = parent->shared_parent();
            }
            return false;
        }
        case Combinator::Child: {
            auto parent = current->shared_parent();
            if (parent == nullptr || !parent->is_element()) {
                return false;
            }
            return match_complex(*parent, selector, index - 1);
        }
        case Combinator::Adjacent:
        case Combinator::Sibling: {
            auto parent = current->shared_parent();
            if (parent == nullptr) {
                return false;
            }
            std::size_t position = 0;
            for (std::size_t k = 0; k < parent->children.size(); ++k) {
                if (parent->children[k].get() == current) {
                    position = k;
                    break;
                }
            }
            if (combinator == Combinator::Adjacent) {
                for (std::size_t k = position; k > 0; --k) {
                    const auto& previous = parent->children[k - 1];
                    if (previous->is_element()) {
                        return match_complex(*previous, selector, index - 1);
                    }
                }
                return false;
            }
            for (std::size_t k = position; k > 0; --k) {
                const auto& previous = parent->children[k - 1];
                if (previous->is_element() &&
                    match_complex(*previous, selector, index - 1)) {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::vector<ComplexSelector> parse_list() {
        std::vector<ComplexSelector> selectors;
        skip_space();
        selectors.push_back(parse_complex());
        skip_space();
        while (i_ < text_.size() && text_[i_] == ',') {
            ++i_;
            skip_space();
            selectors.push_back(parse_complex());
            skip_space();
        }
        if (i_ != text_.size()) {
            throw Error(ErrorCode::ParseError,
                        "unexpected character in selector: " +
                            std::string(1, text_[i_]));
        }
        return selectors;
    }

private:
    void skip_space() {
        while (i_ < text_.size() && is_space(text_[i_])) {
            ++i_;
        }
    }

    bool skip_space_with_flag() {
        const std::size_t before = i_;
        skip_space();
        return i_ != before;
    }

    ComplexSelector parse_complex() {
        ComplexSelector selector;
        selector.compounds.push_back(parse_compound());
        while (i_ < text_.size() && text_[i_] != ',' &&
               text_[i_] != ')') {
            Combinator combinator = Combinator::Descendant;
            bool explicit_combinator = false;
            const std::size_t before = i_;
            skip_space();
            const bool had_space = i_ != before;
            if (i_ < text_.size() && text_[i_] == '>') {
                combinator = Combinator::Child;
                ++i_;
            } else if (i_ < text_.size() && text_[i_] == '+') {
                combinator = Combinator::Adjacent;
                ++i_;
            } else if (i_ < text_.size() && text_[i_] == '~') {
                combinator = Combinator::Sibling;
                ++i_;
            } else if (!had_space) {
                break;
            }
            explicit_combinator = combinator != Combinator::Descendant;
            skip_space();
            if (i_ >= text_.size() || text_[i_] == ',' || text_[i_] == ')') {
                if (explicit_combinator) {
                    throw Error(ErrorCode::ParseError, "missing selector after combinator");
                }
                break;
            }
            selector.combinators.push_back(combinator);
            selector.compounds.push_back(parse_compound());
        }
        return selector;
    }

    Compound parse_compound() {
        Compound compound;
        bool consumed = false;
        if (i_ < text_.size() && text_[i_] == '*') {
            compound.universal = true;
            ++i_;
            consumed = true;
        } else if (i_ < text_.size() && is_ident_char(text_[i_]) &&
                   text_[i_] != '-') {
            compound.tag = to_lower(read_ident(text_, i_));
            consumed = true;
        } else if (i_ < text_.size() && text_[i_] == '-') {
            compound.tag = to_lower(read_ident(text_, i_));
            consumed = true;
        }

        while (i_ < text_.size()) {
            const char c = text_[i_];
            if (c == '.') {
                ++i_;
                compound.classes.push_back(read_ident(text_, i_));
                if (compound.classes.back().empty()) {
                    throw Error(ErrorCode::ParseError, "empty class selector");
                }
                consumed = true;
            } else if (c == '#') {
                ++i_;
                compound.ids.push_back(read_ident(text_, i_));
                if (compound.ids.back().empty()) {
                    throw Error(ErrorCode::ParseError, "empty id selector");
                }
                consumed = true;
            } else if (c == '[') {
                compound.attributes.push_back(parse_attribute());
                consumed = true;
            } else if (c == ':') {
                compound.pseudos.push_back(parse_pseudo());
                consumed = true;
            } else {
                break;
            }
        }

        if (!consumed) {
            throw Error(ErrorCode::ParseError, "empty compound selector");
        }
        return compound;
    }

    AttrSelector parse_attribute() {
        ++i_;  // '['
        skip_space();
        AttrSelector selector;
        selector.name = to_lower(read_ident(text_, i_));
        if (selector.name.empty()) {
            throw Error(ErrorCode::ParseError, "empty attribute name");
        }
        skip_space();
        if (i_ < text_.size() && text_[i_] != ']') {
            if (text_[i_] == '=') {
                selector.op = AttrOp::Equals;
                ++i_;
            } else if (i_ + 1 < text_.size() && text_[i_ + 1] == '=') {
                switch (text_[i_]) {
                    case '~': selector.op = AttrOp::Includes; break;
                    case '|': selector.op = AttrOp::DashMatch; break;
                    case '^': selector.op = AttrOp::Prefix; break;
                    case '$': selector.op = AttrOp::Suffix; break;
                    case '*': selector.op = AttrOp::Substring; break;
                    default:
                        throw Error(ErrorCode::ParseError,
                                    "unknown attribute operator");
                }
                i_ += 2;
            } else {
                throw Error(ErrorCode::ParseError,
                            "unknown attribute operator");
            }
            skip_space();
            if (i_ < text_.size() &&
                (text_[i_] == '"' || text_[i_] == '\'')) {
                const char quote = text_[i_++];
                while (i_ < text_.size() && text_[i_] != quote) {
                    if (text_[i_] == '\\') {
                        ++i_;
                        if (i_ == text_.size()) {
                            throw Error(ErrorCode::ParseError, "invalid attribute escape");
                        }
                    }
                    selector.value.push_back(text_[i_++]);
                }
                if (i_ == text_.size()) {
                    throw Error(ErrorCode::ParseError, "unterminated attribute value");
                }
                ++i_;
            } else {
                selector.value = read_ident(text_, i_);
                if (selector.value.empty()) {
                    throw Error(ErrorCode::ParseError, "missing attribute value");
                }
            }
        }
        skip_space();
        if (i_ >= text_.size() || text_[i_] != ']') {
            throw Error(ErrorCode::ParseError, "unterminated attribute selector");
        }
        ++i_;
        return selector;
    }

    PseudoClass parse_pseudo() {
        ++i_;  // ':'
        PseudoClass pseudo;
        const std::string name = to_lower(read_ident(text_, i_));
        if (name == "first-child") {
            pseudo.kind = PseudoKind::FirstChild;
        } else if (name == "last-child") {
            pseudo.kind = PseudoKind::LastChild;
        } else if (name == "only-child") {
            pseudo.kind = PseudoKind::OnlyChild;
        } else if (name == "empty") {
            pseudo.kind = PseudoKind::Empty;
        } else if (name == "root") {
            pseudo.kind = PseudoKind::Root;
        } else if (name == "first-of-type") {
            pseudo.kind = PseudoKind::FirstOfType;
        } else if (name == "last-of-type") {
            pseudo.kind = PseudoKind::LastOfType;
        } else if (name == "only-of-type") {
            pseudo.kind = PseudoKind::OnlyOfType;
        } else if (name == "nth-child" || name == "nth-last-child" ||
                   name == "nth-of-type" || name == "nth-last-of-type") {
            if (name == "nth-child") {
                pseudo.kind = PseudoKind::NthChild;
            } else if (name == "nth-last-child") {
                pseudo.kind = PseudoKind::NthLastChild;
            } else if (name == "nth-of-type") {
                pseudo.kind = PseudoKind::NthOfType;
            } else {
                pseudo.kind = PseudoKind::NthLastOfType;
            }
            parse_nth(pseudo);
        } else if (name == "not") {
            pseudo.kind = PseudoKind::Not;
            if (i_ >= text_.size() || text_[i_] != '(') {
                throw Error(ErrorCode::ParseError, ":not() requires an argument");
            }
            ++i_;
            skip_space();
            pseudo.not_list.push_back(parse_compound());
            skip_space();
            if (i_ >= text_.size() || text_[i_] != ')') {
                throw Error(ErrorCode::ParseError, "unterminated :not()");
            }
            ++i_;
        } else {
            throw Error(ErrorCode::ParseError, "unsupported pseudo-class: " + name);
        }
        return pseudo;
    }

    void parse_nth(PseudoClass& pseudo) {
        if (i_ >= text_.size() || text_[i_] != '(') {
            throw Error(ErrorCode::ParseError, ":nth-child() requires an argument");
        }
        ++i_;
        skip_space();
        const auto close = text_.find(')', i_);
        if (close == std::string_view::npos) {
            throw Error(ErrorCode::ParseError, "unterminated :nth-child()");
        }
        std::string argument = to_lower(std::string(text_.substr(i_, close - i_)));
        while (!argument.empty() && is_space(argument.back())) {
            argument.pop_back();
        }
        i_ = close + 1;
        const auto integer = [](std::string_view value) {
            if (!value.empty() && value.front() == '+') {
                value.remove_prefix(1);
                if (value.empty() || value.front() == '-') {
                    throw Error(ErrorCode::ParseError, "invalid nth expression");
                }
            }
            int result = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                throw Error(ErrorCode::ParseError, "invalid nth expression");
            }
            return result;
        };
        if (argument == "odd") {
            pseudo.nth_a = 2;
            pseudo.nth_b = 1;
            return;
        }
        if (argument == "even") {
            pseudo.nth_a = 2;
            pseudo.nth_b = 0;
            return;
        }
        const auto n = argument.find('n');
        if (n == std::string::npos) {
            pseudo.nth_a = 0;
            pseudo.nth_b = integer(argument);
            return;
        }
        std::string a = argument.substr(0, n);
        std::string b = argument.substr(n + 1);
        if (a.empty() || a == "+") {
            pseudo.nth_a = 1;
        } else if (a == "-") {
            pseudo.nth_a = -1;
        } else {
            pseudo.nth_a = integer(a);
        }
        const auto first = b.find_first_not_of(" \t\n\r\f");
        if (first == std::string::npos) {
            pseudo.nth_b = 0;
            return;
        }
        b.erase(0, first);
        if (b.front() != '+' && b.front() != '-') {
            throw Error(ErrorCode::ParseError, "missing nth offset sign");
        }
        const char sign = b.front();
        b.erase(0, 1);
        const auto digits = b.find_first_not_of(" \t\n\r\f");
        if (digits == std::string::npos || b[digits] < '0' || b[digits] > '9') {
            throw Error(ErrorCode::ParseError, "missing nth offset");
        }
        b.erase(0, digits);
        pseudo.nth_b = integer(std::string(1, sign) + b);
    }

    std::string_view text_;
    std::size_t i_ = 0;
};

}  // namespace

struct Selector::Impl {
    std::vector<ComplexSelector> selectors;
};

Selector::Selector() = default;
Selector::~Selector() = default;
Selector::Selector(Selector&&) noexcept = default;
Selector& Selector::operator=(Selector&&) noexcept = default;

Selector Selector::parse(std::string_view text) {
    Parser parser(text);
    Selector selector;
    selector.impl_ = std::make_unique<Impl>();
    selector.impl_->selectors = parser.parse_list();
    return selector;
}

bool Selector::matches(const Node& node) const {
    if (impl_ == nullptr) {
        return false;
    }
    for (const auto& complex : impl_->selectors) {
        if (match_complex(node, complex, complex.compounds.size() - 1)) {
            return true;
        }
    }
    return false;
}

namespace {

void collect(const std::shared_ptr<Node>& node, const Selector& selector,
             std::vector<std::shared_ptr<Node>>& out) {
    if (node->is_element() && selector.matches(*node)) {
        out.push_back(node);
    }
    for (const auto& child : node->children) {
        collect(child, selector, out);
    }
}

}  // namespace

std::shared_ptr<Node> query_selector(const std::shared_ptr<Node>& root,
                                     std::string_view selector_text) {
    const auto selector = Selector::parse(selector_text);
    std::vector<std::shared_ptr<Node>> results;
    collect(root, selector, results);
    return results.empty() ? nullptr : results.front();
}

std::vector<std::shared_ptr<Node>> query_selector_all(
    const std::shared_ptr<Node>& root, std::string_view selector_text) {
    const auto selector = Selector::parse(selector_text);
    std::vector<std::shared_ptr<Node>> results;
    collect(root, selector, results);
    return results;
}

}  // namespace prowsetk::flatworm
