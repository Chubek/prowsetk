#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "prowsetk/document.hpp"
#include "prowsetk/ir.hpp"

namespace {

std::string quote_shell(const std::filesystem::path& path) {
    std::string quoted = "'";
    for (const char c : path.string()) quoted += c == '\'' ? "'\\''" : std::string(1, c);
    return quoted + "'";
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(out));
    out << content;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::filesystem::path events_fixture(const std::filesystem::path& directory) {
    const auto document = prowsetk::parse_html(
        "<html><head><title>TeX export</title></head><body>"
        "<h1>Heading</h1><p>A <b>bold</b> <code>code_x</code> "
        "<a href='https://example.test/a?x=1'>link</a>.</p>"
        "<ul><li>first</li><li>second</li></ul></body></html>");
    const auto input = directory / "page.events";
    write_file(input, prowsetk::encode_prowse_events_ndjson(prowsetk::emit_prowse_events(*document)));
    return input;
}

}  // namespace

TEST(Page2Latex, WritesEscapedLatexWithHyperref) {
    const auto directory = std::filesystem::path(TEST_BINARY_DIR) / "page2latex";
    std::filesystem::create_directories(directory);
    const auto input = events_fixture(directory);
    const auto output = directory / "page.tex";
    ASSERT_EQ(std::system((quote_shell(PAGE2LATEX_BIN) + " --hyperref " +
                           quote_shell(input) + " " + quote_shell(output)).c_str()), 0);
    const auto tex = read_file(output);
    EXPECT_NE(tex.find("\\usepackage[hidelinks]{hyperref}"), std::string::npos);
    EXPECT_NE(tex.find("\\section*{Heading}"), std::string::npos);
    EXPECT_NE(tex.find("\\href{https://example.test/a?x=1}{link}"), std::string::npos);
    EXPECT_NE(tex.find("\\texttt{code\\_x}"), std::string::npos);
    EXPECT_NE(tex.find("\\begin{itemize}"), std::string::npos);
}

TEST(Page2Latex, AppliesBodyTemplate) {
    const auto directory = std::filesystem::path(TEST_BINARY_DIR) / "page2latex";
    std::filesystem::create_directories(directory);
    const auto input = events_fixture(directory);
    const auto template_file = directory / "template.tex";
    const auto output = directory / "templated.ltx";
    write_file(template_file,
               "\\documentclass{article}\n{{PAGE2LATEX_PREAMBLE}}"
               "\\begin{document}\nTITLE={{PAGE2LATEX_TITLE}}\n{{PAGE2LATEX_BODY}}\\end{document}\n");
    ASSERT_EQ(std::system((quote_shell(PAGE2LATEX_BIN) + " --template " +
                           quote_shell(template_file) + " " + quote_shell(input) +
                           " " + quote_shell(output)).c_str()), 0);
    const auto tex = read_file(output);
    EXPECT_NE(tex.find("TITLE=TeX export"), std::string::npos);
    EXPECT_NE(tex.find("\\section*{Heading}"), std::string::npos);
}

TEST(Page2Latex, CompilesPdfWithConfiguredEngine) {
#ifndef PAGE2LATEX_ENGINE
    GTEST_SKIP() << "XeLaTeX is not available in this build environment";
#else
    ASSERT_EQ(setenv("PWTK_PAGE2LATEX_ENGINE", PAGE2LATEX_ENGINE, 1), 0);
    const auto directory = std::filesystem::path(TEST_BINARY_DIR) / "page2latex";
    std::filesystem::create_directories(directory);
    const auto input = events_fixture(directory);
    const auto output = directory / "page.pdf";
    ASSERT_EQ(std::system((quote_shell(PAGE2LATEX_BIN) + " --hyperref " +
                           quote_shell(input) + " " + quote_shell(output)).c_str()), 0);
    const auto pdf = read_file(output);
    ASSERT_GE(pdf.size(), 8U);
    EXPECT_EQ(pdf.substr(0, 5), "%PDF-");

    const auto tex_output = directory / "also-make-pdf.tex";
    const auto sibling_pdf = directory / "also-make-pdf.pdf";
    ASSERT_EQ(std::system((quote_shell(PAGE2LATEX_BIN) + " --make-pdf " +
                           quote_shell(input) + " " + quote_shell(tex_output)).c_str()), 0);
    EXPECT_TRUE(std::filesystem::is_regular_file(tex_output));
    EXPECT_EQ(read_file(sibling_pdf).substr(0, 5), "%PDF-");
#endif
}
