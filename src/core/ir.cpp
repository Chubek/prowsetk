#include "prowsetk/ir.hpp"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "flatworm/dom_internal.hpp"
#include "prowsetk/xpath.hpp"

namespace prowsetk {
namespace {

namespace fw = flatworm;

struct IrBuild {
    std::vector<ProwseDomNode> dom_nodes;
    std::vector<ProwseXasEvent> xas_events;
    std::unordered_map<const fw::Node*, std::string> node_paths;
};

bool is_renderable_text(const std::string& text) {
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return true;
        }
    }
    return false;
}

std::string escape_iml(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

std::string element_path(const std::shared_ptr<fw::Node>& node) {
    if (node == nullptr || node->type != fw::NodeType::Element) {
        return {};
    }
    std::vector<std::string> segments;
    auto current = node;
    while (current != nullptr) {
        if (current->type == fw::NodeType::Element) {
            std::size_t index = 1;
            if (auto parent = current->shared_parent()) {
                for (const auto& child : parent->children) {
                    if (child.get() == current.get()) {
                        break;
                    }
                    if (child->type == fw::NodeType::Element &&
                        child->name == current->name) {
                        ++index;
                    }
                }
            }
            segments.push_back(current->name + "[" + std::to_string(index) + "]");
        }
        current = current->shared_parent();
        if (current != nullptr && current->type == fw::NodeType::Document) {
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

void walk_tree(const std::shared_ptr<fw::Node>& node, std::size_t depth,
               IrBuild& out) {
    if (node == nullptr) {
        return;
    }
    if (node->type == fw::NodeType::Document) {
        for (const auto& child : node->children) {
            walk_tree(child, depth, out);
        }
        return;
    }
    if (node->type != fw::NodeType::Element) {
        return;
    }

    const std::string path = element_path(node);
    out.node_paths[node.get()] = path;

    ProwseDomNode dom;
    dom.xpath = path;
    dom.tag = node->name;
    dom.text = fw::text_content(*node);
    dom.depth = depth;
    dom.attributes.reserve(node->attributes.size());
    for (const auto& attr : node->attributes) {
        dom.attributes.push_back(Attribute{attr.name, attr.value});
    }
    out.dom_nodes.push_back(std::move(dom));

    out.xas_events.push_back(
        ProwseXasEvent{"start", path, node->name, {}, {}, depth});
    for (const auto& attr : node->attributes) {
        out.xas_events.push_back(ProwseXasEvent{"attribute", path, node->name,
                                                attr.name, attr.value, depth});
    }

    std::size_t text_index = 0;
    for (const auto& child : node->children) {
        if (child->type == fw::NodeType::Text && is_renderable_text(child->text)) {
            ++text_index;
            out.xas_events.push_back(ProwseXasEvent{
                "text", path + "/text()[" + std::to_string(text_index) + "]",
                node->name, {}, child->text, depth + 1});
        }
        if (child->type == fw::NodeType::Element) {
            walk_tree(child, depth + 1, out);
        }
    }

    out.xas_events.push_back(ProwseXasEvent{"end", path, node->name, {}, {},
                                            depth});
}

IrBuild build_ir(const Document& document) {
    IrBuild build;
    const auto& root = document.raw_root();
    walk_tree(root, 0, build);
    return build;
}

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

bool read_u32(std::span<const std::uint8_t> bytes, std::size_t& pos,
              std::uint32_t& value) {
    if (bytes.size() - pos < 4) {
        return false;
    }
    value = static_cast<std::uint32_t>(bytes[pos]) |
            (static_cast<std::uint32_t>(bytes[pos + 1]) << 8u) |
            (static_cast<std::uint32_t>(bytes[pos + 2]) << 16u) |
            (static_cast<std::uint32_t>(bytes[pos + 3]) << 24u);
    pos += 4;
    return true;
}

bool read_vtd_string(std::span<const std::uint8_t> bytes, std::size_t& pos,
                     std::string& out) {
    std::uint32_t length = 0;
    if (!read_u32(bytes, pos, length) || bytes.size() - pos < length) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes.data() + pos), length);
    pos += length;
    return true;
}

void write_bytes(std::vector<std::uint8_t>& out, std::string_view text) {
    write_u32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

void write_token(std::vector<std::uint8_t>& out, std::uint8_t type,
                 std::uint32_t depth, std::string_view path,
                 std::string_view name, std::string_view value) {
    out.push_back(type);
    write_u32(out, depth);
    write_bytes(out, path);
    write_bytes(out, name);
    write_bytes(out, value);
}

void emit_iml_node(const std::shared_ptr<fw::Node>& node, std::size_t indent,
                   std::ostringstream& stream) {
    if (node == nullptr) {
        return;
    }
    const std::string spaces(indent, ' ');
    if (node->type == fw::NodeType::Document) {
        stream << "(document\n";
        for (const auto& child : node->children) {
            emit_iml_node(child, indent + 2, stream);
        }
        stream << ")\n";
        return;
    }
    if (node->type == fw::NodeType::Element) {
        stream << spaces << "(element " << node->name;
        if (!node->attributes.empty()) {
            stream << " (@";
            for (const auto& attr : node->attributes) {
                stream << " (" << attr.name << " \"" << escape_iml(attr.value)
                       << "\")";
            }
            stream << ")";
        }
        const bool has_element_child = std::any_of(
            node->children.begin(), node->children.end(), [](const auto& child) {
                return child != nullptr && child->type == fw::NodeType::Element;
            });
        const bool has_text_child = std::any_of(
            node->children.begin(), node->children.end(), [](const auto& child) {
                return child != nullptr && child->type == fw::NodeType::Text &&
                       is_renderable_text(child->text);
            });
        if (!has_element_child && !has_text_child) {
            stream << ")\n";
            return;
        }
        stream << "\n";
        for (const auto& child : node->children) {
            emit_iml_node(child, indent + 2, stream);
        }
        stream << spaces << ")\n";
        return;
    }
    if (node->type == fw::NodeType::Text && is_renderable_text(node->text)) {
        stream << spaces << "(text \"" << escape_iml(node->text) << "\")\n";
    }
}

std::string unescape_iml_string(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        const char next = text[++i];
        switch (next) {
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            default:
                out.push_back(next);
                break;
        }
    }
    return out;
}

std::string parse_quoted(std::string_view text, std::size_t& pos) {
    if (pos >= text.size() || text[pos] != '"') {
        return {};
    }
    ++pos;
    std::string raw;
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') {
            return unescape_iml_string(raw);
        }
        if (c == '\\' && pos < text.size()) {
            raw.push_back(c);
            raw.push_back(text[pos++]);
            continue;
        }
        raw.push_back(c);
    }
    return {};
}

void skip_spaces(std::string_view text, std::size_t& pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
            text[pos] == '\r')) {
        ++pos;
    }
}

bool parse_macro(std::string_view text, std::size_t start, std::size_t& end,
                 std::string& name, std::vector<std::string>& args) {
    std::size_t pos = start;
    if (text.substr(pos, 7) != "(macro ") {
        return false;
    }
    pos += 7;
    const std::size_t name_start = pos;
    while (pos < text.size() && text[pos] != ')' && text[pos] != ' ' &&
           text[pos] != '\t' && text[pos] != '\n' && text[pos] != '\r') {
        ++pos;
    }
    if (pos == name_start) {
        return false;
    }
    name.assign(text.substr(name_start, pos - name_start));
    while (true) {
        skip_spaces(text, pos);
        if (pos >= text.size()) {
            return false;
        }
        if (text[pos] == ')') {
            end = pos + 1;
            return true;
        }
        if (text[pos] != '"') {
            return false;
        }
        args.push_back(parse_quoted(text, pos));
    }
}

}  // namespace

std::vector<ProwseXasEvent> emit_prowse_xas(const Document& document) {
    return build_ir(document).xas_events;
}

std::vector<ProwseXasEvent> filter_prowse_xas(const Document& document,
                                              std::string_view xpath_expression) {
    if (xpath_expression.empty()) {
        return {};
    }
    const IrBuild ir = build_ir(document);
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

    std::vector<ProwseXasEvent> filtered;
    for (const auto& event : ir.xas_events) {
        for (const auto& path : selected_paths) {
            if (event.xpath == path ||
                (event.xpath.size() > path.size() &&
                 event.xpath.compare(0, path.size(), path) == 0 &&
                 event.xpath[path.size()] == '/')) {
                filtered.push_back(event);
                break;
            }
        }
    }
    return filtered;
}

std::vector<ProwseDomNode> emit_prowse_dom(const Document& document) {
    return build_ir(document).dom_nodes;
}

std::vector<std::uint8_t> emit_prowse_vtd(const Document& document) {
    const auto events = emit_prowse_xas(document);
    std::vector<std::uint8_t> binary;
    binary.insert(binary.end(), {'P', 'V', 'T', 'D', '1'});
    write_u32(binary, static_cast<std::uint32_t>(events.size()));

    for (const auto& event : events) {
        std::uint8_t type = 0;
        if (event.kind == "start") {
            type = 1;
        } else if (event.kind == "attribute") {
            type = 2;
        } else if (event.kind == "text") {
            type = 3;
        } else if (event.kind == "end") {
            type = 4;
        }
        write_token(binary, type, static_cast<std::uint32_t>(event.depth),
                    event.xpath, event.name.empty() ? event.tag : event.name,
                    event.value);
    }
    return binary;
}

std::vector<ProwseXasEvent> decode_prowse_vtd(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 9 || bytes[0] != 'P' || bytes[1] != 'V' ||
        bytes[2] != 'T' || bytes[3] != 'D' || bytes[4] != '1') {
        return {};
    }
    std::size_t pos = 5;
    std::uint32_t count = 0;
    if (!read_u32(bytes, pos, count)) {
        return {};
    }

    std::vector<ProwseXasEvent> events;
    events.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pos >= bytes.size()) {
            return {};
        }
        const std::uint8_t type = bytes[pos++];
        std::uint32_t depth = 0;
        ProwseXasEvent event;
        if (!read_u32(bytes, pos, depth) ||
            !read_vtd_string(bytes, pos, event.xpath) ||
            !read_vtd_string(bytes, pos, event.name) ||
            !read_vtd_string(bytes, pos, event.value)) {
            return {};
        }
        event.depth = depth;
        switch (type) {
            case 1:
                event.kind = "start";
                event.tag = event.name;
                event.name.clear();
                break;
            case 2:
                event.kind = "attribute";
                break;
            case 3:
                event.kind = "text";
                event.tag = event.name;
                event.name.clear();
                break;
            case 4:
                event.kind = "end";
                event.tag = event.name;
                event.name.clear();
                break;
            default:
                return {};
        }
        events.push_back(std::move(event));
    }
    return pos == bytes.size() ? events : std::vector<ProwseXasEvent>{};
}

std::string emit_prowse_iml(const Document& document) {
    std::ostringstream stream;
    emit_iml_node(document.raw_root(), 0, stream);
    return stream.str();
}

std::string expand_prowse_iml(std::string_view iml,
                              const ProwseImlMacroExpander& expander) {
    std::string out;
    out.reserve(iml.size());
    std::size_t pos = 0;
    while (pos < iml.size()) {
        if (iml[pos] != '(' || iml.substr(pos, 7) != "(macro ") {
            out.push_back(iml[pos++]);
            continue;
        }
        std::size_t end = pos;
        std::string name;
        std::vector<std::string> args;
        if (!parse_macro(iml, pos, end, name, args) || !expander) {
            out.push_back(iml[pos++]);
            continue;
        }
        const std::string expanded = expander(name, args);
        if (expanded.empty()) {
            out.append(iml.substr(pos, end - pos));
        } else {
            out.append(expanded);
        }
        pos = end;
    }
    return out;
}

}  // namespace prowsetk
