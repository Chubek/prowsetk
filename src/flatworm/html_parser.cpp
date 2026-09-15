#include "flatworm/dom_internal.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace prowsetk::flatworm {
namespace {

const std::unordered_map<std::string, std::string>& named_entities() {
    static const std::unordered_map<std::string, std::string> entities = {
        {"amp", "&"},     {"lt", "<"},      {"gt", ">"},
        {"quot", "\""},   {"apos", "'"},    {"nbsp", "\xc2\xa0"},
        {"copy", "\xc2\xa9"}, {"reg", "\xc2\xae"}, {"hellip", "\xe2\x80\xa6"},
        {"mdash", "\xe2\x80\x94"}, {"ndash", "\xe2\x80\x93"},
        {"laquo", "\xc2\xab"}, {"raquo", "\xc2\xbb"},
        {"trade", "\xe2\x84\xa2"}, {"deg", "\xc2\xb0"},
    };
    return entities;
}

std::string to_lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return result;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

bool is_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ||
           c == '_' || c == ':' || c == '.';
}

bool is_void_element(std::string_view name) {
    static const char* const voids[] = {
        "area", "base", "br",  "col",   "embed", "hr",    "img", "input",
        "link", "meta", "param", "source", "track", "wbr"};
    for (const char* element : voids) {
        if (name == element) {
            return true;
        }
    }
    return false;
}

bool is_raw_text_element(std::string_view name) {
    return name == "script" || name == "style";
}

bool is_rcdata_element(std::string_view name) {
    return name == "textarea" || name == "title";
}

std::string decode_entities(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        if (input[i] != '&') {
            output.push_back(input[i++]);
            continue;
        }
        const auto semicolon = input.find(';', i + 1);
        if (semicolon == std::string_view::npos || semicolon - i > 12) {
            output.push_back(input[i++]);
            continue;
        }
        const std::string entity(input.substr(i + 1, semicolon - i - 1));
        if (!entity.empty() && entity[0] == '#') {
            long code = 0;
            bool valid = false;
            try {
                if (entity.size() > 1 &&
                    (entity[1] == 'x' || entity[1] == 'X')) {
                    code = std::stol(entity.substr(2), nullptr, 16);
                } else {
                    code = std::stol(entity.substr(1), nullptr, 10);
                }
                valid = true;
            } catch (...) {
                valid = false;
            }
            if (valid && code > 0 && code < 0x110000) {
                // Encode as UTF-8.
                const auto cp = static_cast<unsigned long>(code);
                if (cp < 0x80) {
                    output.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    output.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    output.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    output.push_back(
                        static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    output.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    output.push_back(
                        static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    output.push_back(
                        static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                i = semicolon + 1;
                continue;
            }
        }
        const auto it = named_entities().find(entity);
        if (it != named_entities().end()) {
            output += it->second;
            i = semicolon + 1;
            continue;
        }
        output.push_back(input[i++]);
    }
    return output;
}

// Elements whose start tag implicitly closes an open element of the same name.
bool closes_previous(std::string_view open, std::string_view incoming) {
    if (open == "li" && incoming == "li") {
        return true;
    }
    if (open == "p" && (incoming == "p" || incoming == "div" ||
                        incoming == "ul" || incoming == "ol" ||
                        incoming == "table" || incoming == "form")) {
        return true;
    }
    if ((open == "td" || open == "th") &&
        (incoming == "td" || incoming == "th" || incoming == "tr")) {
        return true;
    }
    if (open == "tr" && incoming == "tr") {
        return true;
    }
    if (open == "option" && incoming == "option") {
        return true;
    }
    return false;
}

void append_child(const std::shared_ptr<Node>& parent,
                  const std::shared_ptr<Node>& child) {
    child->parent = parent;
    parent->children.push_back(child);
}

}  // namespace

std::shared_ptr<Node> make_node(NodeType type) {
    auto node = std::make_shared<Node>();
    node->type = type;
    return node;
}

const std::string* Node::attribute(std::string_view attribute_name) const {
    for (const auto& attr : attributes) {
        if (attr.name == attribute_name) {
            return &attr.value;
        }
    }
    return nullptr;
}

void Node::set_attribute(std::string_view attribute_name,
                         std::string_view value) {
    for (auto& attr : attributes) {
        if (attr.name == attribute_name) {
            attr.value = std::string(value);
            return;
        }
    }
    attributes.push_back(Attribute{std::string(attribute_name), std::string(value)});
}

bool Node::remove_attribute(std::string_view attribute_name) {
    const auto it = std::remove_if(attributes.begin(), attributes.end(),
                                   [attribute_name](const Attribute& attr) {
                                       return attr.name == attribute_name;
                                   });
    if (it == attributes.end()) {
        return false;
    }
    attributes.erase(it, attributes.end());
    return true;
}

std::string serialize(const Node& node) {
    switch (node.type) {
        case NodeType::Document: {
            std::string result;
            for (const auto& child : node.children) {
                result += serialize(*child);
            }
            return result;
        }
        case NodeType::Text:
            return node.text;
        case NodeType::Comment:
            return "<!--" + node.text + "-->";
        case NodeType::Doctype:
            return "<!" + node.text + ">";
        case NodeType::Element:
            break;
    }

    std::string result = "<" + node.name;
    for (const auto& attr : node.attributes) {
        result += " " + attr.name + "=\"" + attr.value + "\"";
    }
    if (node.children.empty()) {
        result += "></" + node.name + ">";
        return result;
    }
    result += ">";
    for (const auto& child : node.children) {
        result += serialize(*child);
    }
    result += "</" + node.name + ">";
    return result;
}

std::string text_content(const Node& node) {
    if (node.type == NodeType::Text) {
        return node.text;
    }
    std::string result;
    for (const auto& child : node.children) {
        result += text_content(*child);
    }
    return result;
}

std::shared_ptr<Node> parse_html_tree(std::string_view html) {
    auto root = make_node(NodeType::Document);
    std::vector<std::shared_ptr<Node>> stack{root};
    std::size_t i = 0;

    auto current = [&stack]() -> std::shared_ptr<Node>& { return stack.back(); };

    while (i < html.size()) {
        if (html[i] != '<') {
            const auto next = html.find('<', i);
            const auto end = next == std::string_view::npos ? html.size() : next;
            std::string raw(html.substr(i, end - i));
            if (!raw.empty()) {
                auto text = make_node(NodeType::Text);
                text->text = decode_entities(raw);
                append_child(current(), text);
            }
            i = end;
            continue;
        }

        if (html.compare(i, 4, "<!--") == 0) {
            const auto close = html.find("-->", i + 4);
            const auto end = close == std::string_view::npos ? html.size()
                                                             : close + 3;
            auto comment = make_node(NodeType::Comment);
            comment->text = std::string(html.substr(
                i + 4, (close == std::string_view::npos ? html.size() : close) -
                           (i + 4)));
            append_child(current(), comment);
            i = end;
            continue;
        }

        if (html.compare(i, 2, "<!") == 0 || html.compare(i, 2, "<?") == 0) {
            const auto close = html.find('>', i);
            const auto end = close == std::string_view::npos ? html.size()
                                                             : close + 1;
            auto doctype = make_node(NodeType::Doctype);
            const auto content_start = i + 2;
            const auto content_end =
                close == std::string_view::npos ? html.size() : close;
            doctype->text = std::string(
                html.substr(content_start, content_end - content_start));
            append_child(current(), doctype);
            i = end;
            continue;
        }

        if (html.compare(i, 2, "</") == 0) {
            std::size_t j = i + 2;
            std::string name;
            while (j < html.size() && is_name_char(html[j])) {
                name.push_back(html[j++]);
            }
            const auto close = html.find('>', j);
            i = close == std::string_view::npos ? html.size() : close + 1;
            name = to_lower(name);
            for (std::size_t depth = stack.size(); depth > 1; --depth) {
                if (stack[depth - 1]->name == name) {
                    stack.resize(depth - 1);
                    break;
                }
            }
            continue;
        }

        // Start tag.
        std::size_t j = i + 1;
        std::string name;
        while (j < html.size() && is_name_char(html[j])) {
            name.push_back(html[j++]);
        }
        if (name.empty()) {
            // A stray '<' that is not a tag; emit as text.
            auto text = make_node(NodeType::Text);
            text->text = "<";
            append_child(current(), text);
            i += 1;
            continue;
        }
        name = to_lower(name);

        std::vector<Attribute> attributes;
        bool self_closing = false;
        while (j < html.size() && html[j] != '>') {
            while (j < html.size() && is_space(html[j])) {
                ++j;
            }
            if (j < html.size() && html[j] == '/') {
                self_closing = true;
                ++j;
                continue;
            }
            if (j >= html.size() || html[j] == '>') {
                break;
            }
            std::string attr_name;
            while (j < html.size() && is_name_char(html[j])) {
                attr_name.push_back(html[j++]);
            }
            if (attr_name.empty()) {
                ++j;
                continue;
            }
            std::string attr_value;
            while (j < html.size() && is_space(html[j])) {
                ++j;
            }
            if (j < html.size() && html[j] == '=') {
                ++j;
                while (j < html.size() && is_space(html[j])) {
                    ++j;
                }
                if (j < html.size() && (html[j] == '"' || html[j] == '\'')) {
                    const char quote = html[j++];
                    const auto end = html.find(quote, j);
                    const auto stop =
                        end == std::string_view::npos ? html.size() : end;
                    attr_value = decode_entities(html.substr(j, stop - j));
                    j = end == std::string_view::npos ? html.size() : end + 1;
                } else {
                    while (j < html.size() && !is_space(html[j]) &&
                           html[j] != '>') {
                        attr_value.push_back(html[j++]);
                    }
                    attr_value = decode_entities(attr_value);
                }
            }
            attributes.push_back(
                Attribute{to_lower(attr_name), std::move(attr_value)});
        }
        if (j < html.size() && html[j] == '>') {
            ++j;
        }
        i = j;

        auto element = make_node(NodeType::Element);
        element->name = name;
        element->attributes = std::move(attributes);

        while (stack.size() > 1 &&
               closes_previous(stack.back()->name, name)) {
            stack.pop_back();
        }

        append_child(current(), element);

        if (is_raw_text_element(name)) {
            const std::string close_tag = "</" + name;
            std::size_t k = i;
            std::string lowered;
            const auto found = [&]() -> std::size_t {
                std::string hay(html.substr(k));
                std::transform(hay.begin(), hay.end(), hay.begin(), [](char c) {
                    return static_cast<char>(
                        std::tolower(static_cast<unsigned char>(c)));
                });
                return hay.find(close_tag);
            }();
            if (found == std::string::npos) {
                if (k < html.size()) {
                    auto text = make_node(NodeType::Text);
                    text->text = std::string(html.substr(k));
                    append_child(element, text);
                }
                i = html.size();
            } else {
                if (found > 0) {
                    auto text = make_node(NodeType::Text);
                    text->text = std::string(html.substr(k, found));
                    append_child(element, text);
                }
                const auto gt = html.find('>', k + found);
                i = gt == std::string_view::npos ? html.size() : gt + 1;
            }
            continue;
        }

        if (is_rcdata_element(name)) {
            const std::string close_tag = "</" + name;
            std::size_t k = i;
            std::string hay(html.substr(k));
            std::transform(hay.begin(), hay.end(), hay.begin(), [](char c) {
                return static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            });
            const auto found = hay.find(close_tag);
            if (found == std::string::npos) {
                if (k < html.size()) {
                    auto text = make_node(NodeType::Text);
                    text->text = decode_entities(html.substr(k));
                    append_child(element, text);
                }
                i = html.size();
            } else {
                if (found > 0) {
                    auto text = make_node(NodeType::Text);
                    text->text =
                        decode_entities(html.substr(k, found));
                    append_child(element, text);
                }
                const auto gt = html.find('>', k + found);
                i = gt == std::string_view::npos ? html.size() : gt + 1;
            }
            continue;
        }

        if (!self_closing && !is_void_element(name)) {
            stack.push_back(element);
        }
    }

    return root;
}

}  // namespace prowsetk::flatworm
