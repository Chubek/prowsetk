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

TEST(CssSelector, MalformedSelectorThrows) {
    const auto document = parse_html(kHtml);
    EXPECT_THROW(document->query_selector("li["), prowsetk::Error);
    EXPECT_THROW(document->query_selector("li:bogus"), prowsetk::Error);
}
