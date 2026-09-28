#include "flatworm/html_syntax.hpp"
#include <cstdint>
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

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
        out.push_back(static_cast<char>(0x80 | (cp & 63)));
    }
}
}
std::string decode_entities(std::string_view input) {
    static constexpr std::uint32_t c1[] = {
        0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
        0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,
        0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
        0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '&') { output.push_back(input[i++]); continue; }
        if (i + 1 < input.size() && input[i + 1] == '#') {
            auto j = i + 2;
            std::uint32_t base = 10, cp = 0;
            if (j < input.size() && (input[j] == 'x' || input[j] == 'X')) {
                base = 16; ++j;
            }
            const auto first = j;
            while (j < input.size()) {
                const char c = input[j];
                const auto digit = static_cast<std::uint32_t>(
                    c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                    c >= 'A' && c <= 'F' ? c - 'A' + 10 : 255);
                if (digit >= base) break;
                // Saturate before multiplying: arbitrary-length references cannot overflow.
                cp = cp > 0x10ffff / base ? 0x110000 : cp * base + digit;
                ++j;
            }
            if (j != first) {
                if (j < input.size() && input[j] == ';') ++j;
                if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
                else if (cp >= 0x80 && cp <= 0x9f) cp = c1[cp - 0x80];
                append_utf8(output, cp); i = j; continue;
            }
        } else {
            // The supported named vocabulary is small; bound lookahead per ampersand.
            const auto tail = input.substr(i + 1, 12);
            const auto end = tail.find(';');
            if (end != std::string_view::npos) {
                const auto found = named_entities().find(std::string(tail.substr(0, end)));
                if (found != named_entities().end()) {
                    output += found->second; i += end + 2; continue;
                }
            }
        }
        output.push_back(input[i++]);
    }
    return output;
}
}
