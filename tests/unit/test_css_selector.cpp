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
