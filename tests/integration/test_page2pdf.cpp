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

TEST(Page2Pdf, AcceptsVtdFromStandardInput) {
    const std::filesystem::path directory =
        std::filesystem::path(TEST_BINARY_DIR) / "page2pdf";
    std::filesystem::create_directories(directory);
    const auto document = prowsetk::parse_html("<html><body><p>stdin</p></body></html>");
    ASSERT_NE(document, nullptr);
    const auto input = directory / "stdin.vtd";
    const auto output = directory / "stdin.pdf";
    write_bytes(input, prowsetk::emit_prowse_vtd(*document));
    const std::string command = shell_quote(PAGE2PDF_BIN) + " --format vtd - " +
                                shell_quote(output) + " < " + shell_quote(input);
    ASSERT_EQ(std::system(command.c_str()), 0);
    expect_pdf(output);
}

TEST(Page2Pdf, AcceptsImlDocumentTextFromStandardInput) {
    const std::filesystem::path directory =
        std::filesystem::path(TEST_BINARY_DIR) / "page2pdf";
    std::filesystem::create_directories(directory);
    const auto document = prowsetk::parse_html(
        "\xEF\xBB\xBF<html><body><h1>BOM-safe pipeline</h1></body></html>");
    ASSERT_NE(document, nullptr);

    const auto input = directory / "stdin-bom.iml";
    const auto output = directory / "stdin-bom.pdf";
    write_text(input, prowsetk::emit_prowse_iml(*document));
    const std::string command = shell_quote(PAGE2PDF_BIN) + " --format iml - " +
                                shell_quote(output) + " < " + shell_quote(input);
    ASSERT_EQ(std::system(command.c_str()), 0);
    expect_pdf(output);
}

TEST(Page2Pdf, PaintsImageFromBothIrFormats) {
    const auto directory = std::filesystem::path(TEST_BINARY_DIR) / "page2pdf";
    std::filesystem::create_directories(directory);
    const auto document = prowsetk::parse_html(
        "<html><head><style>.panel,.other{background-color:#123456;"
        "border:2px solid red;padding:12px}"
        ".panel h1{color:white;text-align:center}</style></head><body>"
        "<div class='panel'><h1>Graphic</h1>"
        "<img width='72' height='48' src='data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABAQMAAAAl21bKAAAAA1BMVEX/AAAZ4gk3"
        "AAAACklEQVQI12NgAAAAAgAB4iG8MwAAAABJRU5ErkJggg=='></div>"
        "</body></html>");
    ASSERT_NE(document, nullptr);
    const auto iml = directory / "graphics.iml";
    const auto vtd = directory / "graphics.vtd";
    write_text(iml, prowsetk::emit_prowse_iml(*document));
    write_bytes(vtd, prowsetk::emit_prowse_vtd(*document));
    for (const auto& input : {iml, vtd}) {
        const auto output = directory / (input.filename().string() + ".pdf");
        const std::string command = shell_quote(PAGE2PDF_BIN) + " --format " +
            (input == iml ? "iml" : "vtd") + " " + shell_quote(input) +
            " " + shell_quote(output);
        ASSERT_EQ(std::system(command.c_str()), 0);
        expect_pdf(output);
        EXPECT_NE(read_text(output).find("/Subtype /Image"), std::string::npos);
    }
}

TEST(Page2Pdf, SkipsCorruptPngInsteadOfCrashing) {
    const auto directory = std::filesystem::path(TEST_BINARY_DIR) / "page2pdf";
    std::filesystem::create_directories(directory);
    const auto document = prowsetk::parse_html(
        "<html><body><img src='data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/ZY0AAAAASUVORK5CYII='>"
        "</body></html>");
    ASSERT_NE(document, nullptr);
    const auto input = directory / "invalid-image.iml";
    const auto output = directory / "invalid-image.pdf";
    write_text(input, prowsetk::emit_prowse_iml(*document));
    ASSERT_EQ(std::system((shell_quote(PAGE2PDF_BIN) + " --format iml " +
                           shell_quote(input) + " " + shell_quote(output)).c_str()), 0);
    expect_pdf(output);
    EXPECT_EQ(read_text(output).find("/Subtype /Image"), std::string::npos);
}
