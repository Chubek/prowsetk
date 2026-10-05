#include <gtest/gtest.h>
#include <prowsetk/render.hpp>
#include <prowsetk/error.hpp>
#include <algorithm>
#include <limits>

using namespace prowsetk;
namespace {
const PaintItem* item(const RenderedPage& page, std::string_view id, PaintKind kind) {
    for (const auto& p : page.paint.items)
        if (p.element && p.element->id() == id && p.kind == kind) return &p;
    return nullptr;
}
struct MissingImage : ImageLoader {
    unsigned calls = 0;
    std::vector<std::uint8_t> load(std::string_view) override { ++calls; return {}; }
};
}
TEST(Render, CascadeListsImportanceInheritanceAndNonInheritedProperties) {
    auto doc = parse_html(R"(<style>
      div, p { width: 100px; background-color: red; color: blue; }
      #a { width: 120px; } .x { width: 80px !important; }
      p { width: 90px; } p { width: 110px; }
      :unsupported { color: green; }
    </style><div id='a' class='x' style='width: 150px'><span id='b'>hello</span></div><p id='c'>next</p>)");
    const auto styles = resolve_computed_styles(*doc);
    const auto& a = styles.at(*doc->query_selector("#a"));
    const auto& b = styles.at(*doc->query_selector("#b"));
    EXPECT_EQ(a.width.value, 80);
    EXPECT_EQ(a.background_color, (Color{255, 0, 0, 255}));
    EXPECT_EQ(b.color, (Color{0, 0, 255, 255}));
    EXPECT_TRUE(b.width.is_auto);
    EXPECT_EQ(b.background_color.a, 0);
    EXPECT_EQ(styles.at(*doc->query_selector("#c")).width.value, 110);
}
TEST(Render, BlockBoxModelPaintOrderAndClippedHitTesting) {
    auto doc = parse_html(R"(<div id='outer' style='width:100px;height:20px;padding:4px;border:2px solid red;overflow:hidden'>
      <a id='link' href='/next' style='display:block;width:150px;height:50px'>hello</a>
    </div><div id='next' style='width:20px;height:10px'></div>)");
    auto page = render_document(*doc);
    const auto* outer = item(page, "outer", PaintKind::Background);
    const auto* link = item(page, "link", PaintKind::Text);
    const auto* next = item(page, "next", PaintKind::Background);
    ASSERT_NE(outer, nullptr); ASSERT_NE(link, nullptr); ASSERT_NE(next, nullptr);
    EXPECT_DOUBLE_EQ(outer->rect.width, 112);
    EXPECT_DOUBLE_EQ(outer->rect.height, 32);
    EXPECT_DOUBLE_EQ(next->rect.y, 32);
    ASSERT_TRUE(link->clip);
    auto hit = hit_test(page.paint, link->rect.x + 1, link->rect.y + 1);
    ASSERT_NE(hit.interactive, nullptr);
    EXPECT_EQ(hit.interactive->id(), "link");
    EXPECT_EQ(hit_test(page.paint, 140, 10).kind, HitKind::None);
}
TEST(Render, HiddenContentAndPrivateControls) {
    auto doc = parse_html(R"(<div style='display:none'>omitted</div>
      <img hidden src='/hidden'><input type='password' value='private-fixture'>
      <textarea>private-fixture</textarea><details><summary>summary</summary>closed-body</details>)");
    const auto page = render_document(*doc);
    std::string text;
    for (const auto& p : page.paint.items) { text += p.text; EXPECT_NE(p.kind, PaintKind::Image); }
    EXPECT_EQ(text.find("omitted"), std::string::npos);
    EXPECT_EQ(text.find("closed-body"), std::string::npos);
    EXPECT_EQ(text.find("private-fixture"), std::string::npos);
    EXPECT_NE(text.find("summary"), std::string::npos);
}
TEST(Render, Utf8LongWordsAndWhitespace) {
    const auto& measurer = default_text_measurer();
    FontSpec font;
    const std::string text = "abcdefghijklmno";
    auto lines = break_text(text, font, 20, WhiteSpace::Normal, measurer);
    ASSERT_GT(lines.size(), 1u);
    std::string joined;
    for (const auto& [start, end] : lines) {
        const auto part = text.substr(start, end-start);
        EXPECT_LE(measurer.measure(part, font), 20);
        joined += part;
    }
    EXPECT_EQ(joined, text);
    EXPECT_EQ(break_text(text, font, 20, WhiteSpace::NoWrap, measurer).size(), 1u);
    EXPECT_EQ(break_text("a\nb", font, 200, WhiteSpace::Pre, measurer).size(), 2u);
    const auto utf = break_text("ééé", font, 9, WhiteSpace::Normal, measurer);
    for (const auto& [start, end] : utf) { EXPECT_EQ(start % 2, 0u); EXPECT_EQ(end % 2, 0u); }
}
TEST(Render, ImagesAreExplicitCachedAndBounded) {
    auto doc = parse_html("<img id='img' src='https://image.test/p.png' alt='missing'>");
    auto page = render_document(*doc);
    const auto* img = item(page, "img", PaintKind::Image);
    ASSERT_NE(img, nullptr); EXPECT_TRUE(img->degraded);
    MissingImage loader; ImageCache cache; RenderOptions options;
    options.images = &cache; options.loader = &loader;
    render_document(*doc, options); render_document(*doc, options);
    EXPECT_EQ(loader.calls, 1u);
    for (std::size_t i = 0; i < max_render_images + 10; ++i)
        cache.get(loader, "https://image.test/" + std::to_string(i));
    EXPECT_LE(cache.size(), max_render_images);
    EXPECT_EQ(loader.calls, max_render_images);
    EXPECT_GT(cache.dropped(), 0u);
    cache.clear(); EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(ImageBitmap::decode({}), nullptr);
}
TEST(Render, RejectsInvalidOptionsAndStylesheetBudgets) {
    auto doc = parse_html("<p>test</p>");
    RenderOptions options;
    for (double value : {0.0, -1.0, 20000.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
        options.viewport_width = value;
        EXPECT_THROW(render_document(*doc, options), Error);
    }
    EXPECT_THROW(parse_stylesheet(std::string(max_render_stylesheet_bytes + 1, ' ')), Error);
    std::string nested;
    for (int i = 0; i < 34; ++i) nested += "@media screen{";
    nested += "p{color:red}"; nested += std::string(34, '}');
    EXPECT_THROW(parse_stylesheet(nested), Error);
    EXPECT_EQ(parse_stylesheet("@media screen { p,a { color: red } }").size(), 2u);
}

TEST(Render, PngDecodeAndDimensionBudget) {
    std::vector<std::uint8_t> png{
        137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,
        181,28,12,2,0,0,0,11,73,68,65,84,120,218,99,252,255,31,0,3,3,2,0,239,191,101,141,
        0,0,0,0,73,69,78,68,174,66,96,130};
    const auto bitmap = ImageBitmap::decode(png);
#if PROWSETK_TEST_STB
    ASSERT_NE(bitmap, nullptr);
    EXPECT_EQ(bitmap->width(), 1u); EXPECT_EQ(bitmap->height(), 1u);
    EXPECT_EQ(bitmap->rgba().size(), 4u);
#else
    EXPECT_EQ(bitmap, nullptr);
#endif
    png[16] = 0x7f; // oversized IHDR width must be rejected before allocation
    EXPECT_EQ(ImageBitmap::decode(png), nullptr);
    png.resize(20);
    EXPECT_EQ(ImageBitmap::decode(png), nullptr);
}

TEST(Render, OverlapUsesPaintOrderAndDisabledTargetIsNotInteractive) {
    auto doc = parse_html("<button id='first' style='display:block;width:100px;height:30px'>first</button>"
        "<button id='second' disabled style='display:block;position:relative;top:-30px;width:100px;height:30px'>second</button>");
    const auto page = render_document(*doc);
    const auto* second = item(page, "second", PaintKind::Control);
    ASSERT_NE(second, nullptr);
    const auto hit = hit_test(page.paint, second->rect.x + 1, second->rect.y + 1);
    ASSERT_NE(hit.element, nullptr);
    EXPECT_EQ(hit.element->id(), "second");
    EXPECT_EQ(hit.interactive, nullptr);
}

TEST(Render, TooManyElementsFailBeforeLayout) {
    std::string html;
    for (std::size_t i = 0; i <= max_render_elements; ++i) html += "<i></i>";
    auto doc = parse_html(html);
    EXPECT_THROW(render_document(*doc), Error);
}

TEST(Render, BoundsInheritedFontDataAndNonFiniteGeometry) {
    auto doc = parse_html("<div id='a'><span>text</span></div>");
    auto element = doc->query_selector("#a");
    element->set_attribute("style", "font-family:'" + std::string(1025, 'a') + "'");
    EXPECT_THROW(render_document(*doc), Error);
    element->set_attribute("style", "padding:1e7em");
    EXPECT_THROW(render_document(*doc), Error);
    element->set_attribute("style", "width:2rem;height:1px");
    const auto page = render_document(*doc);
    const auto* box = item(page, "a", PaintKind::Background);
    ASSERT_NE(box, nullptr);
    EXPECT_DOUBLE_EQ(box->rect.width, 32);
}
