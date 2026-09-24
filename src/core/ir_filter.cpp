// ir_filter.cpp — legality stratum.
//
// Sole owner of XPath interpretation for IR listeners and walkers. Validates
// expressions, evaluates them against the DOM, and maps matches to canonical
// event selections using layout-stratum paths. Never emits IRs of its own.

#include "ir_internal.hpp"

#include <unordered_set>

#include "prowsetk/error.hpp"
#include "prowsetk/xpath.hpp"

namespace prowsetk {

XPathLegality check_xpath_legality(const Document& document,
                                   std::string_view expression) {
    if (expression.empty()) {
        return {false, "XPath expression is empty"};
    }
    try {
        (void)evaluate_xpath(document, expression);
    } catch (const Error& error) {
        return {false, error.what()};
    } catch (const std::exception& error) {
        return {false, error.what()};
    } catch (...) {
        return {false, "invalid XPath expression"};
    }
    return {true, {}};
}

bool is_valid_xpath_expression(const Document& document,
                               std::string_view expression) {
    return check_xpath_legality(document, expression).ok;
}

std::vector<ProwseXasEvent> filter_prowse_xas(const Document& document,
                                              std::string_view xpath_expression) {
    if (xpath_expression.empty()) {
        return {};
    }
    const ir::IrBuild ir = ir::build_ir(document);
    std::unordered_set<std::string> selected_paths;
    try {
        const XPathValue matches = evaluate_xpath(document, xpath_expression);
        if (matches.type != XPathValueType::NodeSet) {
            return {};
        }
        for (const auto& node : matches.nodes) {
            if (node == nullptr || node->node() == nullptr) {
                continue;
            }
            const auto found = ir.node_paths.find(node->node().get());
            if (found != ir.node_paths.end()) {
                selected_paths.insert(found->second);
            }
        }
    } catch (...) {
        return {};
    }
    if (selected_paths.empty()) {
        return {};
    }

    // Membership is tested per event by walking up through ancestor scopes,
    // so filtering costs O(events * depth) hash lookups instead of
    // O(events * selected).
    std::vector<ProwseXasEvent> filtered;
    filtered.reserve(ir.xas_events.size());
    std::string scope;
    for (const auto& event : ir.xas_events) {
        scope = event.xpath;
        while (true) {
            if (selected_paths.find(scope) != selected_paths.end()) {
                filtered.push_back(event);
                break;
            }
            // Text-node suffixes (".../text()[n]") belong to their parent
            // element scope; strip the suffix before climbing further.
            std::size_t slash = scope.find_last_of('/');
            if (slash == std::string::npos) {
                break;
            }
            if (scope.compare(slash + 1, 7, "text()[") == 0) {
                scope.resize(slash);
                continue;
            }
            // Climb only through indexed element segments ("a[1]"); anything
            // else has no parent scope in the layout scheme.
            const std::size_t bracket = scope.find_last_of('[');
            if (bracket == std::string::npos || bracket < slash + 1 ||
                scope.back() != ']') {
                break;
            }
            scope.resize(slash);
            if (scope.empty()) {
                break;
            }
        }
    }
    return filtered;
}

}  // namespace prowsetk
