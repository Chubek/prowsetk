// ir_events.cpp — event-stream transport encoding stratum.
//
// This file only serializes ProwseEvent records. It deliberately does not
// inspect a Document, assign paths, or select nodes.

#include "prowsetk/ir.hpp"

#include <string_view>

namespace prowsetk {
namespace {

void append_json_string(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4U) & 0x0fU]);
                    out.push_back(hex[c & 0x0fU]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void append_key(std::string& out, std::string_view key) {
    append_json_string(out, key);
    out.push_back(':');
}

}  // namespace

std::string encode_prowse_events_ndjson(std::span<const ProwseEvent> events) {
    std::string out;
    for (const auto& event : events) {
        out.push_back('{');
        append_key(out, "kind"); append_json_string(out, event.kind);
        out.push_back(',');
        append_key(out, "xpath"); append_json_string(out, event.xpath);
        out.push_back(',');
        append_key(out, "tag"); append_json_string(out, event.tag);
        out.push_back(',');
        append_key(out, "name"); append_json_string(out, event.name);
        out.push_back(',');
        append_key(out, "value"); append_json_string(out, event.value);
        out.push_back(',');
        append_key(out, "depth"); out += std::to_string(event.depth);
        if (event.kind == "start") {
            out.push_back(',');
            append_key(out, "text"); append_json_string(out, event.subtree_text);
            out += ",\"attributes\":[";
            for (std::size_t i = 0; i < event.attributes.size(); ++i) {
                if (i != 0) out.push_back(',');
                out += "{\"name\":";
                append_json_string(out, event.attributes[i].name);
                out += ",\"value\":";
                append_json_string(out, event.attributes[i].value);
                out.push_back('}');
            }
            out.push_back(']');
        }
        out += "}\n";
    }
    return out;
}

}  // namespace prowsetk
