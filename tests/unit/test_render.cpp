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

TEST(Render, FlexCascadeShorthandsAndInitialValues) {
    auto doc = parse_html(R"(<style>
      #row { display:flex; gap:8px 12px; column-gap:16px; align-items:center; }
      #a { flex:2 0 60px; flex-grow:3 !important; background:blue; background-color:red; }
    </style><div id='row'><div id='a' style='flex:1;box-sizing:border-box'>A<span id='child'>B</span></div>
    <div id='b' style='flex:none;align-self:flex-end;order:-2'>C</div></div>)");
    const auto styles = resolve_computed_styles(*doc);
    const auto& row = styles.at(*doc->query_selector("#row"));
    EXPECT_EQ(row.display, Display::Flex);
    EXPECT_EQ(row.row_gap.value, 8);
    EXPECT_EQ(row.column_gap.value, 16);
    EXPECT_EQ(row.align_items, AlignItems::Center);
    const auto& a = styles.at(*doc->query_selector("#a"));
    EXPECT_EQ(a.flex_grow, 3);
    EXPECT_EQ(a.flex_shrink, 1);
    EXPECT_EQ(a.flex_basis.value, 0);
    EXPECT_EQ(a.box_sizing, BoxSizing::BorderBox);
    EXPECT_EQ(a.background_color, (Color{255, 0, 0, 255}));
    const auto& b = styles.at(*doc->query_selector("#b"));
    EXPECT_EQ(b.flex_grow, 0);
    EXPECT_EQ(b.flex_shrink, 0);
    EXPECT_TRUE(b.flex_basis.is_auto);
    EXPECT_EQ(b.align_self, AlignItems::End);
    EXPECT_EQ(b.order, -2);
    const auto& child = styles.at(*doc->query_selector("#child"));
    EXPECT_EQ(child.flex_grow, 0);
    EXPECT_TRUE(child.flex_basis.is_auto);
    EXPECT_EQ(child.box_sizing, BoxSizing::ContentBox);
    EXPECT_EQ(child.align_items, AlignItems::Stretch);
    EXPECT_EQ(child.column_gap.value, 0);
    EXPECT_FALSE(child.align_self);
}

TEST(Render, FlexGrowthRedistributesAfterMaximumAndMinimumConstraints) {
    auto doc = parse_html(R"(<div style='display:flex;width:320px;gap:10px'>
      <div id='a' style='flex:1 1 60px;max-width:80px;height:20px'></div>
      <div id='b' style='flex:2 1 60px;height:30px'></div>
      <div id='c' style='flex:1 1 60px;min-width:100px;height:10px'></div>
    </div>)");
    const auto page = render_document(*doc);
    const auto* a = item(page, "a", PaintKind::Background);
    const auto* b = item(page, "b", PaintKind::Background);
    const auto* c = item(page, "c", PaintKind::Background);
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr); ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(a->rect.width, 80);
    EXPECT_DOUBLE_EQ(b->rect.width, 120);
    EXPECT_DOUBLE_EQ(c->rect.width, 100);
    EXPECT_DOUBLE_EQ(b->rect.x, 90);
    EXPECT_DOUBLE_EQ(c->rect.right(), 320);
}

TEST(Render, FlexShrinkUsesScaledBasisAndHonorsMinimum) {
    auto doc = parse_html(R"(<div style='display:flex;width:240px;gap:10px'>
      <div id='a' style='width:200px;min-width:180px;height:10px'></div>
      <div id='b' style='width:100px;height:10px'></div>
    </div>)");
    auto page = render_document(*doc);
    ASSERT_NE(item(page, "a", PaintKind::Background), nullptr);
    ASSERT_NE(item(page, "b", PaintKind::Background), nullptr);
    EXPECT_DOUBLE_EQ(item(page, "a", PaintKind::Background)->rect.width, 180);
    EXPECT_DOUBLE_EQ(item(page, "b", PaintKind::Background)->rect.width, 50);
    doc->query_selector("#a")->set_attribute("style", "width:200px;height:10px");
    page = render_document(*doc);
    EXPECT_NEAR(item(page, "a", PaintKind::Background)->rect.width, 200.0 * 230 / 300, 1e-6);
    EXPECT_NEAR(item(page, "b", PaintKind::Background)->rect.width, 100.0 * 230 / 300, 1e-6);
}

TEST(Render, FlexFractionalGrowthLeavesSpaceForJustification) {
    auto doc = parse_html(R"(<div style='display:flex;width:200px;justify-content:space-between'>
      <div id='a' style='flex:0.25 0 0px;height:10px'></div>
      <div id='b' style='flex:0.25 0 0px;height:10px'></div>
    </div>)");
    const auto page = render_document(*doc);
    ASSERT_NE(item(page, "a", PaintKind::Background), nullptr);
    ASSERT_NE(item(page, "b", PaintKind::Background), nullptr);
    EXPECT_DOUBLE_EQ(item(page, "a", PaintKind::Background)->rect.width, 50);
    EXPECT_DOUBLE_EQ(item(page, "b", PaintKind::Background)->rect.x, 150);
}

TEST(Render, FlexWrappingReverseOrderAndCrossAxisAlignment) {
    auto doc = parse_html(R"(<div id='row' style='display:flex;flex-wrap:wrap;flex-direction:row-reverse;width:210px;gap:12px 10px;align-items:center'>
      <div id='a' style='flex:0 0 100px;height:20px'></div>
      <div id='b' style='flex:0 0 100px;height:40px;order:-1'></div>
      <div id='c' style='flex:0 0 100px;height:30px'></div>
    </div><div id='after' style='height:10px'></div>)");
    const auto page = render_document(*doc);
    const auto* a = item(page, "a", PaintKind::Background);
    const auto* b = item(page, "b", PaintKind::Background);
    const auto* c = item(page, "c", PaintKind::Background);
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr); ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(b->rect.x, 110); EXPECT_DOUBLE_EQ(b->rect.y, 0);
    EXPECT_DOUBLE_EQ(a->rect.x, 0); EXPECT_DOUBLE_EQ(a->rect.y, 10);
    EXPECT_DOUBLE_EQ(c->rect.x, 110); EXPECT_DOUBLE_EQ(c->rect.y, 52);
    ASSERT_NE(item(page, "after", PaintKind::Background), nullptr);
    EXPECT_DOUBLE_EQ(item(page, "after", PaintKind::Background)->rect.y, 82);
    EXPECT_LT(b, a); // order also defines paint order
}

TEST(Render, FlexStretchPreservesNestedImageAndMovesLocalClips) {
    auto doc = parse_html(R"(<div style='display:flex;width:300px;height:100px;align-items:stretch;justify-content:flex-end;overflow:hidden'>
      <div id='card' style='width:100px;max-height:60px;padding:5px;border:1px solid black;overflow:hidden'>
        <a href='/next' id='link' style='display:block;width:200px;height:80px'>Link</a>
        <img id='img' style='width:10px;height:7px'>
      </div>
    </div>)");
    const auto page = render_document(*doc);
    const auto* card = item(page, "card", PaintKind::Background);
    const auto* link = item(page, "link", PaintKind::Background);
    const auto* img = item(page, "img", PaintKind::Image);
    ASSERT_NE(card, nullptr); ASSERT_NE(link, nullptr); ASSERT_NE(img, nullptr);
    EXPECT_DOUBLE_EQ(card->rect.x, 188);
    EXPECT_DOUBLE_EQ(card->rect.height, 72);
    EXPECT_DOUBLE_EQ(img->rect.height, 7);
    ASSERT_TRUE(link->clip);
    EXPECT_DOUBLE_EQ(link->clip->x, 194);
    EXPECT_DOUBLE_EQ(link->clip->width, 100);
    EXPECT_DOUBLE_EQ(link->clip->height, 60);
    auto hit = hit_test(page.paint, 195, 8);
    ASSERT_NE(hit.interactive, nullptr);
    EXPECT_EQ(hit.interactive->id(), "link");
    EXPECT_EQ(hit_test(page.paint, 305, 8).kind, HitKind::None);
    EXPECT_EQ(hit_test(page.paint, 195, 70).interactive, nullptr);
}

TEST(Render, FlexNestedRowsAnonymousTextAndHiddenChildren) {
    auto doc = parse_html(R"(<div style='display:flex;width:300px;gap:10px'>
      <div hidden style='width:900px'>Hidden</div>
      <div id='nested' style='display:flex;flex:1;gap:10px'>
        <span id='a' style='flex:1;height:20px'>A</span><span id='b' style='flex:1;height:20px'>B</span>
      </div><div id='fixed' style='width:90px;height:40px'>X</div>
    </div><div style='display:flex'>Anonymous <span id='text'>text</span></div>)");
    const auto page = render_document(*doc);
    ASSERT_NE(item(page, "nested", PaintKind::Background), nullptr);
    ASSERT_NE(item(page, "a", PaintKind::Background), nullptr);
    ASSERT_NE(item(page, "b", PaintKind::Background), nullptr);
    EXPECT_DOUBLE_EQ(item(page, "nested", PaintKind::Background)->rect.width, 200);
    EXPECT_DOUBLE_EQ(item(page, "nested", PaintKind::Background)->rect.height, 40);
    EXPECT_DOUBLE_EQ(item(page, "a", PaintKind::Background)->rect.width, 95);
    EXPECT_DOUBLE_EQ(item(page, "b", PaintKind::Background)->rect.x, 105);
    std::string text;
    for (const auto& p : page.paint.items) text += p.text;
    EXPECT_NE(text.find("Anonymous"), std::string::npos);
    EXPECT_EQ(text.find("Hidden"), std::string::npos);
}

TEST(Render, BorderBoxConstraintsAutoMarginsAndFlexAutoMargins) {
    auto doc = parse_html(R"(<div style='width:400px'>
      <div id='box' style='box-sizing:border-box;width:300px;max-width:200px;height:50px;padding:10px;border:2px solid red;margin:0 auto'></div>
      <div style='display:flex;gap:10px'>
        <div id='left' style='width:50px;height:10px'></div>
        <div id='right' style='width:100px;height:10px;margin-left:auto'></div>
      </div></div>)");
    const auto page = render_document(*doc);
    const auto* box = item(page, "box", PaintKind::Background);
    ASSERT_NE(box, nullptr);
    EXPECT_DOUBLE_EQ(box->rect.x, 100);
    EXPECT_DOUBLE_EQ(box->rect.width, 200);
    EXPECT_DOUBLE_EQ(box->rect.height, 50);
    ASSERT_NE(item(page, "right", PaintKind::Background), nullptr);
    EXPECT_DOUBLE_EQ(item(page, "right", PaintKind::Background)->rect.x, 300);
}

TEST(Render, TextAlignmentMovesWholeLinesAndInlineBoxes) {
    auto doc = parse_html(R"(<div style='width:200px;text-align:center'>
      <span id='a'>Hello</span><span id='b'> world</span><br>
      <span id='box' style='display:inline-block;width:40px;height:10px;background:red'></span>
      <div id='block' style='width:50px;height:10px'></div>
    </div><div id='end' style='width:200px;text-align:right'>Right</div>)");
    const auto page = render_document(*doc);
    const auto* a = item(page, "a", PaintKind::Text);
    const auto* b = item(page, "b", PaintKind::Text);
    ASSERT_NE(a, nullptr); ASSERT_NE(b, nullptr);
    EXPECT_NEAR(a->rect.x, 200 - b->rect.right(), 1e-6);
    ASSERT_NE(item(page, "box", PaintKind::Background), nullptr);
    ASSERT_NE(item(page, "block", PaintKind::Background), nullptr);
    ASSERT_NE(item(page, "end", PaintKind::Text), nullptr);
    EXPECT_DOUBLE_EQ(item(page, "box", PaintKind::Background)->rect.x, 80);
    EXPECT_DOUBLE_EQ(item(page, "block", PaintKind::Background)->rect.x, 0);
    EXPECT_DOUBLE_EQ(item(page, "end", PaintKind::Text)->rect.right(), 200);
}

TEST(Render, FlexControlsStayRedactedAndInvalidValuesStayBounded) {
    auto doc = parse_html(R"(<div style='display:flex;width:300px;gap:-10px;flex-grow:1e99'>
      <input id='input' value='private-fixture' style='flex:1;padding:2px;box-sizing:border-box'>
      <div id='a' style='flex:-1;order:1e99;width:20px;height:10px'></div>
    </div>)");
    const auto styles = resolve_computed_styles(*doc);
    const auto& a = styles.at(*doc->query_selector("#a"));
    EXPECT_EQ(a.flex_grow, 0); EXPECT_EQ(a.order, 0);
    EXPECT_TRUE(a.flex_basis.is_auto);
    const auto page = render_document(*doc);
    const auto* control = item(page, "input", PaintKind::Control);
    ASSERT_NE(control, nullptr);
    EXPECT_EQ(control->text, "[redacted]");
    for (const auto& p : page.paint.items) {
        EXPECT_EQ(p.text.find("private-fixture"), std::string::npos);
        EXPECT_GE(p.rect.width, 0);
    }
}

TEST(Render, FlexRepeatedIntrinsicMeasurementHasAnAggregateBudget) {
    std::string html;
    for (int i = 0; i < 20; ++i) html += "<div style='display:flex'>";
    html += std::string(1024 * 1024, 'x');
    for (int i = 0; i < 20; ++i) html += "</div>";
    const auto doc = parse_html(html);
    try { render_document(*doc); FAIL() << "expected an intrinsic measurement limit"; }
    catch (const Error& error) { EXPECT_EQ(error.code(), ErrorCode::ResourceLimit); }
}
