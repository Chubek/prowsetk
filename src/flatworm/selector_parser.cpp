#include "flatworm/selector_model.hpp"
#include "prowsetk/error.hpp"

#include <cctype>
#include <charconv>
#include <cstdint>

namespace prowsetk::flatworm::selectors {
namespace {
bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' ||
           c == '_' || c == '\\' ||
           static_cast<unsigned char>(c) >= 0x80;
}

bool ident_start(std::string_view text, std::size_t i) {
    if (i >= text.size()) return false;
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '-') return i + 1 < text.size() &&
        (text[i + 1] == '-' || ident_start(text, i + 1));
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c >= 0x80 || c == '\\';
}

void append_codepoint(std::string& out, std::uint32_t cp) {
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

void read_escape(std::string_view text, std::size_t& i, std::string& out) {
    ++i; // backslash
    if (i == text.size() || text[i] == '\n' || text[i] == '\r' || text[i] == '\f')
        throw Error(ErrorCode::ParseError, "invalid selector escape");
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex(text[i]) < 0) { out.push_back(text[i++]); return; }
    std::uint32_t cp = 0;
    for (int count = 0; count < 6 && i < text.size() && hex(text[i]) >= 0; ++count)
        cp = cp * 16 + static_cast<std::uint32_t>(hex(text[i++]));
    if (i < text.size() && is_space(text[i])) {
        if (text[i++] == '\r' && i < text.size() && text[i] == '\n') ++i;
    }
    append_codepoint(out, cp);
}

std::string read_ident(std::string_view text, std::size_t& i) {
    std::string result;
    if (!ident_start(text, i)) return result;
    while (i < text.size() && is_ident_char(text[i])) {
        if (text[i] == '\\') read_escape(text, i, result);
        else result.push_back(text[i++]);
    }
    return result;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::vector<ComplexSelector> parse_list(bool nested = false) {
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
        if (i_ != text_.size() && !(nested && text_[i_] == ')')) {
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
        if (++compound_count_ > 256) {
            throw Error(ErrorCode::ParseError, "selector exceeds 256 compounds");
        }
        Compound compound;
        bool consumed = false;
        if (i_ < text_.size() && text_[i_] == '*') {
            compound.universal = true;
            ++i_;
            consumed = true;
        } else if (ident_start(text_, i_)) {
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
                        if (i_ + 1 < text_.size() &&
                            (text_[i_ + 1] == '\n' || text_[i_ + 1] == '\r' || text_[i_ + 1] == '\f')) {
                            ++i_;
                            if (text_[i_++] == '\r' && i_ < text_.size() && text_[i_] == '\n') ++i_;
                        } else read_escape(text_, i_, selector.value);
                    } else {
                        if (text_[i_] == '\n' || text_[i_] == '\r' || text_[i_] == '\f')
                            throw Error(ErrorCode::ParseError, "newline in attribute string");
                        selector.value.push_back(text_[i_++]);
                    }
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
        if (selector.op != AttrOp::Exists && i_ < text_.size() && text_[i_] != ']') {
            const auto flag = to_lower(read_ident(text_, i_));
            if (flag != "i" && flag != "s")
                throw Error(ErrorCode::ParseError, "invalid attribute case flag");
            selector.ascii_insensitive = flag == "i";
            if (selector.ascii_insensitive) selector.value = to_lower(selector.value);
            skip_space();
        }
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
            if (++nesting_ > 32)
                throw Error(ErrorCode::ParseError, "selector nesting exceeds 32");
            pseudo.not_list = parse_list(true);
            --nesting_;
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
    std::size_t compound_count_ = 0;
    std::size_t nesting_ = 0;
};

}  // namespace

std::vector<ComplexSelector> parse(std::string_view text) {
    if (text.size() > 65536) {
        throw Error(ErrorCode::ParseError, "selector exceeds 64 KiB limit");
    }
    return Parser(text).parse_list();
}
}  // namespace prowsetk::flatworm::selectors
