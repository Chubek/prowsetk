#include <gtest/gtest.h>

#include "prowsetk/document.hpp"
#include "prowsetk/error.hpp"

using prowsetk::parse_html;

namespace {
const char* kHtml = R"HTML(
<div id="root">
  <ul class="list">
    <li class="item first">one</li>
    <li class="item">two</li>
    <li class="item last">three</li>
  </ul>
  <a href="https://example.com/a" data-kind="external">A</a>
  <a href="/local" data-kind="internal">B</a>
  <input type="text" disabled>
</div>)HTML";
}

TEST(CssSelector, TypeClassAndId) {
    const auto document = parse_html(kHtml);
    EXPECT_EQ(document->query_selector_all("li").size(), 3u);
    EXPECT_EQ(document->query_selector_all(".item").size(), 3u);
    EXPECT_EQ(document->query_selector_all("li.first").size(), 1u);
    EXPECT_NE(document->query_selector("#root"), nullptr);
    EXPECT_EQ(document->query_selector_all("li.missing").size(), 0u);
}

TEST(CssSelector, AttributeOperators) {
    const auto document = parse_html(kHtml);
    EXPECT_EQ(document->query_selector_all("a[href]").size(), 2u);
    EXPECT_EQ(document->query_selector_all("a[data-kind='external']").size(), 1u);
    EXPECT_EQ(document->query_selector_all("a[href^='https']").size(), 1u);
    EXPECT_EQ(document->query_selector_all("a[href$='/local']").size(), 1u);
    EXPECT_EQ(document->query_selector_all("a[href*='example']").size(), 1u);
    EXPECT_EQ(document->query_selector_all("input[disabled]").size(), 1u);
}

TEST(CssSelector, StructuralPseudoClasses) {
    const auto document = parse_html(kHtml);
    EXPECT_EQ(document->query_selector_all("li:first-child").size(), 1u);
    EXPECT_EQ(document->query_selector_all("li:last-child").size(), 1u);
    EXPECT_EQ(document->query_selector_all("li:nth-child(2)").size(), 1u);
    EXPECT_EQ(document->query_selector_all("li:nth-child(odd)").size(), 2u);
    EXPECT_EQ(document->query_selector_all("li:nth-child(2n)").size(), 1u);
}

TEST(CssSelector, Combinators) {
    const auto document = parse_html(kHtml);
    EXPECT_EQ(document->query_selector_all("ul li").size(), 3u);
    EXPECT_EQ(document->query_selector_all("ul > li").size(), 3u);
    EXPECT_EQ(document->query_selector_all("div > a").size(), 2u);
    EXPECT_EQ(document->query_selector_all("li + li").size(), 2u);
    EXPECT_EQ(document->query_selector_all("li ~ li").size(), 2u);
    EXPECT_EQ(document->query_selector_all("ul > a").size(), 0u);
}

TEST(CssSelector, CommaGroupsAndNot) {
    const auto document = parse_html(kHtml);
    EXPECT_EQ(document->query_selector_all("a, li").size(), 5u);
    EXPECT_EQ(document->query_selector_all("li:not(.first)").size(), 2u);
}

TEST(CssSelector, NthLastChildCountsElementsFromEnd) {
    const auto document = parse_html("<ul><li>A</li>text<li>B</li><!--x--><li>C</li></ul>");
    const auto matches = document->query_selector_all("li:nth-last-child(2)");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front()->text(), "B");
}

TEST(CssSelector, UniversalSelectorAndEmptySubstring) {
    const auto document =
        parse_html("<a href=\"a.html\">A</a><b>B</b><div id=\"empty-sub\"></div>");
    EXPECT_EQ(document->query_selector_all("*").size(), 3u);
    EXPECT_EQ(document->query_selector_all("*.item").size(), 0u);
    EXPECT_EQ(document->query_selector_all("a[href*='']").size(), 0u);
    EXPECT_EQ(document->query_selector_all("*[id]").size(), 1u);
}

TEST(CssSelector, MalformedSelectorThrows) {
    const auto document = parse_html(kHtml);
    EXPECT_THROW(document->query_selector("li["), prowsetk::Error);
    EXPECT_THROW(document->query_selector("li:bogus"), prowsetk::Error);
}

TEST(CssSelector, OfTypePseudos) {
    const auto document = parse_html(
        "<div><p>first</p><span>a</span><p>second</p><p>third</p></div>");
    EXPECT_EQ(document->query_selector_all("p:first-of-type").size(), 1u);
    EXPECT_EQ(document->query_selector_all("p:last-of-type").size(), 1u);
    EXPECT_EQ(document->query_selector_all("p:nth-of-type(2)").size(), 1u);
    EXPECT_EQ(document->query_selector_all("p:nth-last-of-type(2)").size(), 1u);
    EXPECT_EQ(document->query_selector_all("span:only-of-type").size(), 1u);
    EXPECT_EQ(document->query_selector_all("div:only-of-type").size(), 1u);
}

TEST(CssSelector, RootPseudo) {
    const auto document = parse_html("<html><body><div></div></body></html>");
    EXPECT_EQ(document->query_selector_all(":root").size(), 1u);
    EXPECT_EQ(document->query_selector(":root")->tag_name(), "html");
    EXPECT_EQ(document->query_selector_all("html:root").size(), 1u);
    EXPECT_EQ(document->query_selector_all("body:root").size(), 0u);
}

TEST(CssSelector, NotWithCompoundSelectors) {
    const auto document = parse_html(
        "<ul><li class='a'></li><li class='b'></li><li class='a b'></li></ul>");
    EXPECT_EQ(document->query_selector_all("li:not(.a)").size(), 1u);
    EXPECT_EQ(document->query_selector_all("li:not(.b)").size(), 1u);
    EXPECT_EQ(document->query_selector_all("li:not(.a.b)").size(), 2u);
    EXPECT_EQ(document->query_selector_all("li:not([class])").size(), 0u);
}

TEST(CssSelector, NthChildWithNegativeAndZero) {
    const auto document = parse_html(
        "<ol><li>1</li><li>2</li><li>3</li><li>4</li><li>5</li></ol>");
    EXPECT_EQ(document->query_selector_all("li:nth-child(-n+3)").size(), 3u);
    EXPECT_EQ(document->query_selector_all("li:nth-child(3n-1)").size(), 2u);
    EXPECT_EQ(document->query_selector_all("li:nth-child(0)").size(), 0u);
}

TEST(CssSelector, ComplexCombinatorChains) {
    const auto document = parse_html(
        "<div><section><article><p>A</p></article></section>"
        "<section><p>B</p></section></div>");
    EXPECT_EQ(document->query_selector_all("div > section > article > p").size(), 1u);
    EXPECT_EQ(document->query_selector_all("div section article p").size(), 1u);
    EXPECT_EQ(document->query_selector_all("div > section p").size(), 2u);
    EXPECT_EQ(document->query_selector_all("section + section p").size(), 1u);
    EXPECT_EQ(document->query_selector_all("section ~ section p").size(), 1u);
}

TEST(CssSelector, CaseInsensitiveTagMatching) {
    const auto document = parse_html("<DIV><SPAN></SPAN></DIV>");
    EXPECT_EQ(document->query_selector_all("div").size(), 1u);
    EXPECT_EQ(document->query_selector_all("span").size(), 1u);
    EXPECT_EQ(document->query_selector_all("DIV SPAN").size(), 1u);
}

TEST(CssSelector, UnicodeAndHexEscapes) {
    const auto document = parse_html(
        "<div id='123' class='a:b' data-label='café'></div>"
        "<p id='😀'></p><span id='�'></span>");
    EXPECT_NE(document->query_selector(R"(#\31 23)"), nullptr);
    EXPECT_NE(document->query_selector(R"(.a\:b)"), nullptr);
    EXPECT_NE(document->query_selector(R"([data-label='caf\e9 '])"), nullptr);
    EXPECT_NE(document->query_selector(R"(#\1f600)"), nullptr);
    EXPECT_NE(document->query_selector(R"(#\0)"), nullptr);
    EXPECT_NE(document->query_selector(R"(#\d800)"), nullptr);
    EXPECT_NE(document->query_selector(R"(#\110000)"), nullptr);
    EXPECT_NE(document->query_selector("#\\31\r\n23"), nullptr);
    EXPECT_THROW(document->query_selector("#123"), prowsetk::Error);
    EXPECT_THROW(document->query_selector(".\\\n"), prowsetk::Error);
    EXPECT_THROW(document->query_selector("[data-label='caf\né']"), prowsetk::Error);
    EXPECT_NE(document->query_selector("[data-label='caf\\\né']"), nullptr);
}

TEST(CssSelector, ExplicitAttributeCaseFlags) {
    const auto document = parse_html("<div data-value='AbC-DeF xyz' data-unicode='Ä'></div>");
    for (const auto* selector : {"[data-value='abc-def XYZ' i]",
         "[data-value='abc-def XYZ'i]", "[data-value^=abc I]", "[data-value$=XYZ i]", "[data-value*=C-d i]",
         "[data-value~=XYZ i]", "[data-value|=abc i]"}) {
        EXPECT_EQ(document->query_selector_all(selector).size(), 1u) << selector;
    }
    EXPECT_EQ(document->query_selector_all("[data-value^=abc s]").size(), 0u);
    EXPECT_EQ(document->query_selector_all("[data-value^=AbC s]").size(), 1u);
    EXPECT_EQ(document->query_selector_all("[data-value^=abc]").size(), 0u);
    EXPECT_EQ(document->query_selector_all("[data-unicode='ä' i]").size(), 0u);
    EXPECT_EQ(document->query_selector_all("[data-value*='' i]").size(), 0u);
    EXPECT_THROW(document->query_selector("[data-value i]"), prowsetk::Error);
    EXPECT_THROW(document->query_selector("[data-value=x unknown]"), prowsetk::Error);
}

TEST(CssSelector, NegationListsAndComplexSelectors) {
    const auto document = parse_html(
        "<section><p id='a' class='hidden'></p><p id='b'></p></section>"
        "<p id='c'></p><p id='d'></p>");
    auto matches = document->query_selector_all("p:not(section > p, #d)");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front()->id(), "c");
    EXPECT_EQ(document->query_selector_all("p:not(:not(section p))").size(), 2u);
    EXPECT_EQ(document->query_selector_all("p:not(.hidden + p, #d)").size(), 2u);
    for (const auto* selector : {":not()", ":not(p,)", ":not(,p)", ":not(p >)",
                                 ":not(p, :bogus)", ":not(p", "p,"})
        EXPECT_THROW(document->query_selector(selector), prowsetk::Error) << selector;
}

TEST(CssSelector, BoundedSyntax) {
    const auto document = parse_html("<div></div>");
    std::string nested = "div";
    for (int i = 0; i < 32; ++i) nested = ":not(" + nested + ")";
    EXPECT_NO_THROW(document->query_selector(nested));
    EXPECT_THROW(document->query_selector(":not(" + nested + ")"), prowsetk::Error);
    EXPECT_THROW(document->query_selector(std::string(65537, 'a')), prowsetk::Error);
    std::string chain = "div";
    for (int i = 0; i < 256; ++i) chain += " div";
    EXPECT_THROW(document->query_selector(chain), prowsetk::Error);
}

TEST(CssSelector, TraversalOrderAndElementScope) {
    const auto document = parse_html("<div id='root'><p id='a'><b id='b'></b></p><b id='c'></b></div>");
    const auto root = document->query_selector("#root");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->query_selector("#root"), nullptr);
    EXPECT_EQ(root->query_selector("b")->id(), "b");
    auto matches = root->query_selector_all("b, p, #b");
    ASSERT_EQ(matches.size(), 3u);
    EXPECT_EQ(matches[0]->id(), "a");
    EXPECT_EQ(matches[1]->id(), "b");
    EXPECT_EQ(matches[2]->id(), "c");
    EXPECT_TRUE(matches[1]->matches("div > p b:not(#c, .missing)"));
}

TEST(CssSelector, CombinatorialBacktrackingFailsWithResourceLimit) {
    std::string html = "<p id='first'></p>";
    for (int i = 0; i < 35; ++i) html += "<div>";
    for (int i = 0; i < 35; ++i) html += "</div>";
    const auto document = parse_html(html);
    const auto selector = "#first, missing div div div div div div div div div div";
    ASSERT_NE(document->query_selector(selector), nullptr);
    EXPECT_EQ(document->query_selector(selector)->id(), "first");
    try {
        document->query_selector_all(selector);
        FAIL() << "adversarial selector should exhaust its matching budget";
    } catch (const prowsetk::Error& error) {
        EXPECT_EQ(error.code(), prowsetk::ErrorCode::ResourceLimit);
    }
}
