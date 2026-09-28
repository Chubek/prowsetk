// page2latex -- render canonical ProwseEvent NDJSON as a standalone LaTeX document.
// This downstream tool intentionally has no Flatworm or ProwseTk C++ linkage.

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxInputBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaxNodes = 200000U;
constexpr std::size_t kMaxNesting = 512U;

struct Event {
    std::string kind, tag, name, value;
    std::size_t depth = 0;
};

struct Node {
    std::string tag;
    std::string text;
    std::map<std::string, std::string> attributes;
    std::vector<Node> children;
};

struct Options {
    bool hyperref = false;
    bool make_pdf = false;
    std::optional<fs::path> template_path;
    std::string input;
    fs::path output;
};

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void usage(std::ostream& out) {
    out << "Usage: page2latex [--hyperref] [--template FILE] [--make-pdf] INPUT OUTPUT.{tex,ltx,pdf}\n"
           "Convert canonical ProwseEvent NDJSON to LaTeX. INPUT may be '-'.\n"
           "PDF output, or --make-pdf, invokes $PWTK_PAGE2LATEX_ENGINE or xelatex.\n"
           "Templates must contain {{PAGE2LATEX_BODY}}; optional markers are\n"
           "{{PAGE2LATEX_PREAMBLE}} and {{PAGE2LATEX_TITLE}}.\n";
}

std::string read_all(const std::string& path) {
    std::istream* source = &std::cin;
    std::ifstream file;
    if (path != "-") {
        file.open(path, std::ios::binary);
        if (!file) fail("cannot open input '" + path + "'");
        source = &file;
    }
    std::string result;
    char buffer[8192];
    while (source->read(buffer, sizeof(buffer)) || source->gcount() != 0) {
        const auto count = static_cast<std::size_t>(source->gcount());
        if (count > kMaxInputBytes - result.size()) fail("input exceeds the 16 MiB safety limit");
        result.append(buffer, count);
    }
    if (source->bad()) fail("failed to read input");
    return result;
}

class Json {
public:
    explicit Json(std::string_view input) : input_(input) {}

    Event event() {
        Event out;
        bool kind = false, tag = false, name = false, value = false, depth = false;
        take('{');
        skip_space();
        while (!consume('}')) {
            const auto key = string();
            take(':');
            if (key == "kind") { out.kind = string(); kind = true; }
            else if (key == "tag") { out.tag = string(); tag = true; }
            else if (key == "name") { out.name = string(); name = true; }
            else if (key == "value") { out.value = string(); value = true; }
            else if (key == "depth") { out.depth = number(); depth = true; }
            else skip_value(0);
            skip_space();
            if (consume('}')) break;
            take(',');
        }
        skip_space();
        if (pos_ != input_.size()) error("trailing JSON data");
        if (!kind || !tag || !name || !value || !depth) error("missing ProwseEvent field");
        return out;
    }

private:
    std::string_view input_;
    std::size_t pos_ = 0;

    [[noreturn]] void error(std::string_view message) const {
        fail("invalid ProwseEvent JSON: " + std::string(message) + " at byte " + std::to_string(pos_));
    }
    void skip_space() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
    }
    bool consume(char expected) {
        skip_space();
        if (pos_ < input_.size() && input_[pos_] == expected) { ++pos_; return true; }
        return false;
    }
    void take(char expected) { if (!consume(expected)) error("unexpected JSON syntax"); }
    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    void append_codepoint(std::string& out, unsigned value) {
        if (value <= 0x7fU) out.push_back(static_cast<char>(value));
        else if (value <= 0x7ffU) { out.push_back(static_cast<char>(0xc0U | (value >> 6U))); out.push_back(static_cast<char>(0x80U | (value & 0x3fU))); }
        else { out.push_back(static_cast<char>(0xe0U | (value >> 12U))); out.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU))); out.push_back(static_cast<char>(0x80U | (value & 0x3fU))); }
    }
    std::string string() {
        take('"');
        std::string out;
        while (pos_ < input_.size()) {
            const unsigned char c = static_cast<unsigned char>(input_[pos_++]);
            if (c == '"') return out;
            if (c < 0x20U) error("control character in JSON string");
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (pos_ == input_.size()) error("unterminated JSON escape");
            const char escaped = input_[pos_++];
            switch (escaped) {
                case '"': out.push_back('"'); break; case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break; case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break; case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break; case 't': out.push_back('\t'); break;
                case 'u': {
                    if (input_.size() - pos_ < 4U) error("short JSON unicode escape");
                    unsigned codepoint = 0;
                    for (int i = 0; i != 4; ++i) { const int digit = hex(input_[pos_++]); if (digit < 0) error("bad JSON unicode escape"); codepoint = (codepoint << 4U) | static_cast<unsigned>(digit); }
                    if (codepoint >= 0xd800U && codepoint <= 0xdfffU) error("unsupported JSON surrogate");
                    append_codepoint(out, codepoint);
                    break;
                }
                default: error("bad JSON escape");
            }
        }
        error("unterminated JSON string");
    }
    std::size_t number() {
        skip_space(); const auto begin = pos_; std::size_t out = 0;
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
            const auto digit = static_cast<unsigned>(input_[pos_++] - '0');
            if (out > (static_cast<std::size_t>(-1) - digit) / 10U) error("depth overflow");
            out = out * 10U + digit;
        }
        if (begin == pos_ || out > kMaxNesting) error("invalid depth");
        return out;
    }
    void skip_value(unsigned nesting) {
        if (nesting > 64U) error("JSON nesting limit");
        skip_space(); if (pos_ == input_.size()) error("missing JSON value");
        if (input_[pos_] == '"') { static_cast<void>(string()); return; }
        const char open = input_[pos_];
        if (open == '{' || open == '[') {
            ++pos_; const char close = open == '{' ? '}' : ']'; skip_space();
            if (consume(close)) return;
            while (true) {
                if (open == '{') { static_cast<void>(string()); take(':'); }
                skip_value(nesting + 1U); skip_space();
                if (consume(close)) return;
                take(',');
            }
        }
        const auto begin = pos_;
        while (pos_ < input_.size() && input_[pos_] != ',' && input_[pos_] != '}' && input_[pos_] != ']' && !std::isspace(static_cast<unsigned char>(input_[pos_]))) ++pos_;
        if (begin == pos_) error("invalid JSON value");
    }
};

std::vector<Node> parse_events(std::string_view input) {
    std::vector<Node> roots;
    std::vector<Node*> stack;
    std::size_t nodes = 0U, line_number = 0U, start = 0U;
    if (input.starts_with("\xEF\xBB\xBF")) start = 3U;
    while (start < input.size()) {
        const auto end = input.find('\n', start);
        auto line = input.substr(start, (end == std::string_view::npos ? input.size() : end) - start);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
        start = end == std::string_view::npos ? input.size() : end + 1U;
        if (line.empty()) continue;
        ++line_number;
        Event event;
        try { event = Json(line).event(); }
        catch (const std::exception& e) { fail("event record " + std::to_string(line_number) + ": " + e.what()); }
        if (event.kind == "start") {
            if (event.depth != stack.size() || stack.size() >= kMaxNesting || ++nodes > kMaxNodes) fail("invalid start-event depth");
            Node node; node.tag = event.tag;
            if (stack.empty()) { roots.push_back(std::move(node)); stack.push_back(&roots.back()); }
            else { stack.back()->children.push_back(std::move(node)); stack.push_back(&stack.back()->children.back()); }
        } else if (event.kind == "attribute") {
            if (stack.empty() || event.depth + 1U != stack.size() || stack.back()->tag != event.tag) fail("invalid attribute event");
            stack.back()->attributes.emplace(event.name, event.value);
        } else if (event.kind == "text") {
            if (stack.empty() || event.depth != stack.size() || stack.back()->tag != event.tag || ++nodes > kMaxNodes) fail("invalid text event");
            Node text; text.tag = "#text"; text.text = std::move(event.value); stack.back()->children.push_back(std::move(text));
        } else if (event.kind == "end") {
            if (stack.empty() || event.depth + 1U != stack.size() || stack.back()->tag != event.tag) fail("invalid end event");
            stack.pop_back();
        } else fail("unknown event kind");
    }
    if (roots.empty() || !stack.empty()) fail("incomplete ProwseEvent stream");
    return roots;
}

std::string tex_escape(std::string_view input) {
    std::string out;
    for (const char c : input) {
        switch (c) {
            case '\\': out += "\\textbackslash{}"; break; case '{': out += "\\{"; break;
            case '}': out += "\\}"; break; case '#': out += "\\#"; break;
            case '$': out += "\\$"; break; case '%': out += "\\%"; break;
            case '&': out += "\\&"; break; case '_': out += "\\_"; break;
            case '^': out += "\\textasciicircum{}"; break; case '~': out += "\\textasciitilde{}"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string plain_text(const Node& node) {
    if (node.tag == "#text") return node.text;
    std::string out; for (const auto& child : node.children) out += plain_text(child); return out;
}

class LatexWriter {
public:
    explicit LatexWriter(bool hyperref) : hyperref_(hyperref) {}
    std::string render(const std::vector<Node>& roots) { for (const auto& node : roots) render_node(node); return out_.str(); }
private:
    bool hyperref_;
    std::ostringstream out_;
    void children(const Node& node) { for (const auto& child : node.children) render_node(child); }
    std::string attribute(const Node& node, std::string_view name) const { const auto it = node.attributes.find(std::string(name)); return it == node.attributes.end() ? std::string{} : it->second; }
    void render_node(const Node& node) {
        if (node.tag == "#text") { out_ << tex_escape(node.text); return; }
        if (node.tag == "head" || node.tag == "title" || node.tag == "style" || node.tag == "script") return;
        if (node.tag == "h1") { out_ << "\\section*{"; children(node); out_ << "}\n"; return; }
        if (node.tag == "h2") { out_ << "\\subsection*{"; children(node); out_ << "}\n"; return; }
        if (node.tag == "h3") { out_ << "\\subsubsection*{"; children(node); out_ << "}\n"; return; }
        if (node.tag == "p" || node.tag == "div" || node.tag == "section" || node.tag == "article" || node.tag == "header" || node.tag == "footer") { children(node); out_ << "\n\n"; return; }
        if (node.tag == "br") { out_ << "\\\\\n"; return; }
        if (node.tag == "hr") { out_ << "\\par\\noindent\\rule{\\linewidth}{0.4pt}\\par\n"; return; }
        if (node.tag == "b" || node.tag == "strong") { out_ << "\\textbf{"; children(node); out_ << "}"; return; }
        if (node.tag == "i" || node.tag == "em") { out_ << "\\emph{"; children(node); out_ << "}"; return; }
        if (node.tag == "code" || node.tag == "kbd") { out_ << "\\texttt{" << tex_escape(plain_text(node)) << "}"; return; }
        if (node.tag == "pre") { out_ << "\\begin{verbatim}\n" << plain_text(node) << "\n\\end{verbatim}\n"; return; }
        if (node.tag == "ul" || node.tag == "ol") { out_ << "\\begin{" << (node.tag == "ul" ? "itemize" : "enumerate") << "}\n"; children(node); out_ << "\\end{" << (node.tag == "ul" ? "itemize" : "enumerate") << "}\n"; return; }
        if (node.tag == "li") { out_ << "\\item "; children(node); out_ << "\n"; return; }
        if (node.tag == "blockquote") { out_ << "\\begin{quote}"; children(node); out_ << "\\end{quote}\n"; return; }
        if (node.tag == "a") { const auto href = attribute(node, "href"); if (!href.empty() && hyperref_) { out_ << "\\href{" << tex_escape(href) << "}{"; children(node); out_ << "}"; } else { children(node); if (!href.empty()) out_ << "\\footnote{\\url{" << tex_escape(href) << "}}"; } return; }
        if (node.tag == "img") { const auto alt = attribute(node, "alt"); out_ << "\\textit{[image" << (alt.empty() ? "" : ": " + tex_escape(alt)) << "]}"; return; }
        children(node);
    }
};

std::string find_title(const std::vector<Node>& nodes) {
    for (const auto& node : nodes) { if (node.tag == "title") return plain_text(node); const auto nested = find_title(node.children); if (!nested.empty()) return nested; }
    return {};
}

std::string replace_all(std::string value, std::string_view marker, std::string_view replacement) {
    std::size_t pos = 0U; while ((pos = value.find(marker, pos)) != std::string::npos) { value.replace(pos, marker.size(), replacement); pos += replacement.size(); } return value;
}

std::string default_document(std::string_view title, std::string_view body, bool hyperref) {
    std::ostringstream out;
    out << "\\documentclass[11pt]{article}\n\\usepackage[margin=1in]{geometry}\n\\usepackage{xcolor}\n\\usepackage{graphicx}\n\\usepackage{longtable}\n\\usepackage{enumitem}\n\\usepackage{url}\n";
    if (hyperref) out << "\\usepackage[hidelinks]{hyperref}\n";
    out << "\\begin{document}\n";
    if (!title.empty()) out << "\\title{" << tex_escape(title) << "}\n\\date{}\n\\maketitle\n";
    out << body << "\n\\end{document}\n";
    return out.str();
}

void write_file(const fs::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary); if (!out) fail("cannot write '" + path.string() + "'"); out << content; if (!out) fail("failed while writing '" + path.string() + "'");
}

std::string shell_quote(const fs::path& value) {
    std::string out = "'"; for (const char c : value.string()) out += c == '\'' ? "'\\''" : std::string(1, c); return out + "'";
}

void compile_pdf(const fs::path& tex, const fs::path& pdf) {
    const char* configured = std::getenv("PWTK_PAGE2LATEX_ENGINE");
    const std::string engine = configured != nullptr && *configured != '\0' ? configured : "xelatex";
    const auto temp = fs::temp_directory_path() / ("prowsetk-page2latex-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp);
    const std::string command = engine + " -interaction=nonstopmode -halt-on-error -output-directory=" + shell_quote(temp) + " " + shell_quote(tex);
    const int status = std::system(command.c_str());
    const auto generated = temp / (tex.stem().string() + ".pdf");
    if (status != 0 || !fs::is_regular_file(generated)) { std::error_code ignored; fs::remove_all(temp, ignored); fail("LaTeX engine failed; command was: " + engine); }
    std::error_code copy_error;
    fs::copy_file(generated, pdf, fs::copy_options::overwrite_existing, copy_error);
    std::error_code cleanup_error;
    fs::remove_all(temp, cleanup_error);
    if (copy_error) fail("cannot write PDF '" + pdf.string() + "'");
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") { usage(std::cout); std::exit(EXIT_SUCCESS); }
        if (arg == "--hyperref") { options.hyperref = true; continue; }
        if (arg == "--make-pdf") { options.make_pdf = true; continue; }
        if (arg == "--template") { if (++i == argc) fail("--template requires a file"); options.template_path = fs::path(argv[i]); continue; }
        if (arg.starts_with("--")) fail("unknown option '" + std::string(arg) + "'");
        if (options.input.empty()) options.input = std::string(arg); else if (options.output.empty()) options.output = fs::path(arg); else fail("too many arguments");
    }
    if (options.input.empty() || options.output.empty()) fail("INPUT and OUTPUT are required");
    const auto extension = options.output.extension().string();
    if (extension != ".tex" && extension != ".ltx" && extension != ".pdf") fail("OUTPUT must end in .tex, .ltx, or .pdf");
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto roots = parse_events(read_all(options.input));
        const auto title = find_title(roots);
        const auto body = LatexWriter(options.hyperref).render(roots);
        std::string latex = default_document(title, body, options.hyperref);
        if (options.template_path) {
            auto custom = read_all(options.template_path->string());
            if (custom.find("{{PAGE2LATEX_BODY}}") == std::string::npos) fail("template is missing {{PAGE2LATEX_BODY}}");
            custom = replace_all(std::move(custom), "{{PAGE2LATEX_PREAMBLE}}", "\\usepackage[margin=1in]{geometry}\n\\usepackage{xcolor}\n\\usepackage{graphicx}\n\\usepackage{longtable}\n\\usepackage{enumitem}\n\\usepackage{url}\n" + std::string(options.hyperref ? "\\usepackage[hidelinks]{hyperref}\n" : ""));
            latex = replace_all(std::move(custom), "{{PAGE2LATEX_TITLE}}", tex_escape(title));
            latex = replace_all(std::move(latex), "{{PAGE2LATEX_BODY}}", body);
        }
        const bool output_pdf = options.output.extension() == ".pdf";
        const fs::path tex_path = output_pdf ? fs::temp_directory_path() / ("prowsetk-page2latex-source-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tex") : options.output;
        write_file(tex_path, latex);
        if (output_pdf || options.make_pdf) {
            const fs::path pdf_path = output_pdf ? options.output : options.output.parent_path() / (options.output.stem().string() + ".pdf");
            compile_pdf(tex_path, pdf_path);
        }
        if (output_pdf) { std::error_code ignored; fs::remove(tex_path, ignored); }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "page2latex: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
