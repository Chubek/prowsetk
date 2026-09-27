#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "prowsetk/document.hpp"
#include "prowsetk/ir.hpp"

namespace {

std::string shell_quote(const std::filesystem::path& path) {
    return "\"" + path.string() + "\"";
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void expect_pdf(const std::filesystem::path& output) {
    ASSERT_TRUE(std::filesystem::exists(output));
    const std::string pdf = read_text(output);
    ASSERT_GE(pdf.size(), 8U);
    EXPECT_EQ(pdf.substr(0, 5), "%PDF-");
}

}  // namespace

TEST(Page2Pdf, CompilesProwseVtdAndProwseIml) {
    const std::filesystem::path directory =
        std::filesystem::path(TEST_BINARY_DIR) / "page2pdf";
    std::filesystem::create_directories(directory);
    const auto document = prowsetk::parse_html(
        "<html><body><h1>PDF title</h1><p>Hello <b>PDF</b>.</p>"
        "<ul><li>first</li><li>second</li></ul></body></html>");
    ASSERT_NE(document, nullptr);

    const auto vtd_input = directory / "page.vtd";
    const auto vtd_output = directory / "page-vtd.pdf";
    write_bytes(vtd_input, prowsetk::emit_prowse_vtd(*document));
    const std::string vtd_command = shell_quote(PAGE2PDF_BIN) + " --format vtd " +
                                    shell_quote(vtd_input) + " " + shell_quote(vtd_output);
    ASSERT_EQ(std::system(vtd_command.c_str()), 0);
    expect_pdf(vtd_output);

    const auto iml_input = directory / "page.iml";
    const auto iml_output = directory / "page-iml.pdf";
    write_text(iml_input, prowsetk::emit_prowse_iml(*document));
    const std::string iml_command = shell_quote(PAGE2PDF_BIN) + " --format iml " +
                                    shell_quote(iml_input) + " " + shell_quote(iml_output);
    ASSERT_EQ(std::system(iml_command.c_str()), 0);
    expect_pdf(iml_output);
}

TEST(Page2Pdf, RejectsMalformedVtd) {
    const std::filesystem::path directory =
        std::filesystem::path(TEST_BINARY_DIR) / "page2pdf";
    std::filesystem::create_directories(directory);
    const auto input = directory / "bad.vtd";
    const auto output = directory / "bad.pdf";
    write_text(input, "PVTD1\x01");
    std::filesystem::remove(output);
    const std::string command = shell_quote(PAGE2PDF_BIN) + " --format vtd " +
                                shell_quote(input) + " " + shell_quote(output);
    EXPECT_NE(std::system(command.c_str()), 0);
    EXPECT_FALSE(std::filesystem::exists(output));
}
