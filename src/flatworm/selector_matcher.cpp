#include "flatworm/selector_model.hpp"
#include "flatworm/dom_internal.hpp"
#include "prowsetk/error.hpp"

#include <algorithm>
#include <cstdint>

namespace prowsetk::flatworm::selectors {
namespace {
struct MatchBudget {
    std::size_t remaining = 100000;
    void step() {
        if (remaining == 0)
            throw Error(ErrorCode::ResourceLimit, "selector matching budget exhausted");
        --remaining;
    }
};
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

bool match_compound(const Node& node, const Compound& compound, MatchBudget& budget);
bool match_complex(const Node& node, const ComplexSelector& selector, std::size_t index, MatchBudget& budget);

bool match_pseudo(const Node& node, const PseudoClass& pseudo, MatchBudget& budget) {
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
            for (const auto& selector : pseudo.not_list) {
                if (match_complex(node, selector, selector.compounds.size() - 1, budget)) {
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
    std::string normalized;
    if (attr.ascii_insensitive) {
        normalized = to_lower(*value);
        value = &normalized;
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

bool match_compound(const Node& node, const Compound& compound, MatchBudget& budget) {
    budget.step();
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
        if (!match_pseudo(node, pseudo, budget)) {
            return false;
        }
    }
    return true;
}

bool match_complex(const Node& node, const ComplexSelector& selector,
                   std::size_t index, MatchBudget& budget) {
    if (!match_compound(node, selector.compounds[index], budget)) {
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
                if (match_complex(*parent, selector, index - 1, budget)) {
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
            return match_complex(*parent, selector, index - 1, budget);
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
                        return match_complex(*previous, selector, index - 1, budget);
                    }
                }
                return false;
            }
            for (std::size_t k = position; k > 0; --k) {
                const auto& previous = parent->children[k - 1];
                if (previous->is_element() &&
                    match_complex(*previous, selector, index - 1, budget)) {
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

}  // namespace

bool matches(const Node& node, const std::vector<ComplexSelector>& selectors) {
    MatchBudget budget;
    for (const auto& selector : selectors) {
        if (match_complex(node, selector, selector.compounds.size() - 1, budget)) return true;
    }
    return false;
}
}  // namespace prowsetk::flatworm::selectors
