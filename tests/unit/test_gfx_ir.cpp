#include <gtest/gtest.h>
#include "prowsetk/gfx_ir.hpp"
#include "prowsetk/error.hpp"

using namespace prowsetk;
namespace {
std::string preview(std::string_view html, GfxOptions options = {}) {
    auto document = parse_html(html);
    const auto frame = decode_gfx_frame(emit_gfx_ir(emit_prowse_events(*document), options));
    std::string text;
    for (const auto& command : frame.text) text += command.text + "\n";
    return text;
}
}

TEST(GfxIr, SemanticPreviewAndUtf8Wrapping) {
    EXPECT_EQ(preview("<h1>Hello</h1><p>World <b>again</b></p>"), "Hello\nWorld again\n");
    EXPECT_EQ(preview("<p>éééé</p>", {48, 600}), "éé\néé\n");
    EXPECT_EQ(preview("<p>Hello</p>", {1, 1}), "H\ne\nl\nl\no\n");
}

TEST(GfxIr, ExcludesPrivateControlsAndHiddenAncestors) {
    const auto text = preview(
        "<head><title>SECRET_TITLE</title></head><p>Visible</p>"
        "<script>SECRET_SCRIPT</script><style>SECRET_STYLE</style>"
        "<template>SECRET_TEMPLATE</template><div hidden><p>SECRET_HIDDEN</p></div>"
        "<div style='DISPLAY: none'><b>SECRET_DISPLAY</b></div>"
        "<input value='SECRET_INPUT' data-token='SECRET_ATTR'>"
        "<textarea>SECRET_TEXTAREA</textarea><select><option>SECRET_OPTION</option></select>");
    EXPECT_NE(text.find("Visible"), std::string::npos);
    EXPECT_NE(text.find("[input]"), std::string::npos);
    EXPECT_EQ(text.find("SECRET"), std::string::npos);
}

TEST(GfxIr, ValidatesCanonicalEventStructure) {
    auto document = parse_html("<p>Text</p>");
    auto events = emit_prowse_events(*document);
    events.pop_back();
    EXPECT_THROW(emit_gfx_ir(events), Error);
    events = emit_prowse_events(*document);
    events.back().tag = "div";
    EXPECT_THROW(emit_gfx_ir(events), Error);
    events = emit_prowse_events(*document);
    events[1].depth = 99;
    EXPECT_THROW(emit_gfx_ir(events), Error);
    events[0].kind = "unknown";
    EXPECT_THROW(emit_gfx_ir(events), Error);
    EXPECT_THROW(emit_gfx_ir({}, {0, 600}), Error);
}

TEST(GfxIr, BytecodeRoundTripsAndRejectsAllTruncations) {
    GfxFrame frame{{640, 480}, {{16, 16, "Hello é"}, {16, 34, "Second"}}};
    const auto bytes = encode_gfx_frame(frame);
    const auto decoded = decode_gfx_frame(bytes);
    EXPECT_EQ(decoded.text, frame.text);
    EXPECT_EQ(decoded.surface.width, 640u);
    EXPECT_EQ(encode_gfx_frame(decoded), bytes);
    for (std::size_t length = 0; length < bytes.size(); ++length)
        EXPECT_THROW(decode_gfx_frame(std::span(bytes).first(length)), Error) << length;
    auto bad = bytes; bad.push_back(0);
    EXPECT_THROW(decode_gfx_frame(bad), Error);
    bad = bytes; bad[0] = 'X';
    EXPECT_THROW(decode_gfx_frame(bad), Error);
    bad = bytes; bad[17] = 255;
    EXPECT_THROW(decode_gfx_frame(bad), Error);
    bad = bytes;
    for (std::size_t i = 13; i < 17; ++i) bad[i] = 255;
    EXPECT_THROW(decode_gfx_frame(bad), Error);
    bad = bytes;
    for (std::size_t i = 26; i < 30; ++i) bad[i] = 255;
    EXPECT_THROW(decode_gfx_frame(bad), Error);
}

TEST(GfxIr, RejectsInvalidUtf8CoordinatesAndBudgets) {
    for (const auto& invalid : {std::string("\x80"), std::string("\xc0\xaf"),
         std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"), std::string("\xe2\x82"),
         std::string("a\0b", 3)}) {
        EXPECT_THROW(encode_gfx_frame({{}, {{0, 0, invalid}}}), Error);
    }
    EXPECT_THROW(encode_gfx_frame({{}, {{800, 0, "outside"}}}), Error);
    EXPECT_THROW(encode_gfx_frame({{}, {{0, max_gfx_content_height + 1, "outside"}}}), Error);
    EXPECT_THROW(encode_gfx_frame({{}, {{0, 0, std::string(max_gfx_text_bytes + 1, 'a')}}}), Error);
    EXPECT_THROW(encode_gfx_frame({{}, std::vector<GfxText>(max_gfx_commands + 1)}), Error);
    GfxFrame oversized{{}, std::vector<GfxText>(1100, GfxText{0, 0, std::string(4096, 'a')})};
    EXPECT_THROW(encode_gfx_frame(oversized), Error);
}
