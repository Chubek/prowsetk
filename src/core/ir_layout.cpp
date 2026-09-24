// ir_layout.cpp — register/layout stratum.
//
// Sole owner of stable position assignment: XPath-like paths, sibling
// indexes, and depths. No other stratum computes positions; encoders and the
// legality filter consume these assignments verbatim so every IR agrees on
// where a node lives.

#include "ir_internal.hpp"

#include <algorithm>
#include <string>

namespace prowsetk::ir {

bool is_renderable_text(const std::string& text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return true;
        }
    }
    return false;
}

std::string element_path(const std::shared_ptr<flatworm::Node>& node) {
    if (node == nullptr || node->type != flatworm::NodeType::Element) {
        return {};
    }
    std::vector<std::string> segments;
    auto current = node;
    while (current != nullptr) {
        if (current->type == flatworm::NodeType::Element) {
            std::size_t index = 1;
            if (auto parent = current->shared_parent()) {
                for (const auto& child : parent->children) {
                    if (child.get() == current.get()) {
                        break;
                    }
                    if (child->type == flatworm::NodeType::Element &&
                        child->name == current->name) {
                        ++index;
                    }
                }
            }
            segments.push_back(current->name + "[" + std::to_string(index) + "]");
        }
        current = current->shared_parent();
        if (current != nullptr && current->type == flatworm::NodeType::Document) {
            break;
        }
    }
    std::reverse(segments.begin(), segments.end());
    std::string path;
    for (const auto& segment : segments) {
        path.push_back('/');
        path += segment;
    }
    return path;
}

std::string tag_from_path(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string_view::npos || slash + 1 >= path.size()) {
        return {};
    }
    std::string_view segment = path.substr(slash + 1);
    // Text-node suffixes ("text()[n]") and attribute suffixes ("@name") carry
    // no element tag.
    if (segment.starts_with("text()") || segment.starts_with("@")) {
        return {};
    }
    const std::size_t bracket = segment.find('[');
    if (bracket != std::string_view::npos) {
        segment = segment.substr(0, bracket);
    }
    return std::string(segment);
}

bool is_within_scope(std::string_view candidate, std::string_view scope) {
    if (candidate == scope) {
        return true;
    }
    return candidate.size() > scope.size() &&
           candidate.compare(0, scope.size(), scope) == 0 &&
           candidate[scope.size()] == '/';
}

}  // namespace prowsetk::ir
