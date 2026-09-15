#include <gtest/gtest.h>

#include "prowsetk/document.hpp"
#include "prowsetk/error.hpp"
#include "prowsetk/xpath.hpp"

using prowsetk::Error;
using prowsetk::Element;
using prowsetk::XPathValueType;
using prowsetk::evaluate_xpath;
using prowsetk::parse_html;
using prowsetk::xpath_string_value;

#ifdef PROWSETK_HAVE_PUGIXML

namespace {
const char* kHtml = R"HTML(<html>
<head><title>XPath Demo</title></head>
<body>
  <h1 id="heading" class="title big">Hello</h1>
  <ul id="menu">
    <li class="item" data-id="1"><a href="/one">One</a></li>
    <li class="item active" data-id="2"><a href="/two">Two</a></li>
    <li class="item" data-id="3"><a href="/three">Three</a></li>
  </ul>
  <p class="blurb">First paragraph</p>
  <p class="blurb">Second <b>bold</b> paragraph</p>
  <form action="/submit" method="POST">
    <input name="user" value="alice">
    <input type="password" name="password" value="secret">
  </form>
</body>
</html>)HTML";
}

TEST(XPath, SelectsElementsByTag) {
    const auto document = parse_html(kHtml, "https://example.com/");
    const auto value = evaluate_xpath(*document, "//li");
    EXPECT_EQ(value.type, XPathValueType::NodeSet);
    ASSERT_EQ(value.nodes.size(), 3u);
    EXPECT_EQ(value.nodes[0]->tag_name(), "li");
    EXPECT_EQ(value.nodes[0]->attribute("data-id"), "1");
}

TEST(XPath, SelectsDescendantsWithPredicates) {
    const auto document = parse_html(kHtml, "https://example.com/");
    const auto value = evaluate_xpath(*document, "//li[@class='item active']");
    ASSERT_EQ(value.nodes.size(), 1u);
    EXPECT_EQ(value.nodes[0]->attribute("data-id"), "2");

    const auto second = evaluate_xpath(*document, "//li[2]");
    ASSERT_EQ(second.nodes.size(), 1u);
    EXPECT_EQ(second.nodes[0]->attribute("data-id"), "2");
}

TEST(XPath, AttributeExpressionsCarryStringValues) {
    const auto document = parse_html(kHtml, "https://example.com/");
    const auto value = evaluate_xpath(*document, "//a/@href");
    ASSERT_EQ(value.nodes.size(), 3u);
    ASSERT_EQ(value.string_values.size(), 3u);
    EXPECT_EQ(value.string_values[0], "/one");
    EXPECT_EQ(value.string_values[1], "/two");
    EXPECT_EQ(value.string_values[2], "/three");
}

TEST(XPath, ReturnsScalarValues) {
    const auto document = parse_html(kHtml, "https://example.com/");
    const auto count = evaluate_xpath(*document, "count(//li)");
    EXPECT_EQ(count.type, XPathValueType::Number);
    EXPECT_DOUBLE_EQ(count.number_value, 3.0);

    const auto title = evaluate_xpath(*document, "string(//title)");
    EXPECT_EQ(title.type, XPathValueType::String);
    EXPECT_EQ(title.string_value, "XPath Demo");

    const auto exists = evaluate_xpath(*document, "boolean(//form[@method='POST'])");
    EXPECT_EQ(exists.type, XPathValueType::Boolean);
    EXPECT_TRUE(exists.boolean_value);
}

TEST(XPath, ScopedToElementSubtree) {
    const auto document = parse_html(kHtml, "https://example.com/");
    auto menu = document->query_selector("#menu");
    ASSERT_NE(menu, nullptr);
    const auto value = evaluate_xpath(*menu, "li[last()]");
    ASSERT_EQ(value.nodes.size(), 1u);
    EXPECT_EQ(value.nodes[0]->attribute("data-id"), "3");
}

TEST(XPath, TextAndContentHelpers) {
    const auto document = parse_html(kHtml, "https://example.com/");
    const auto text = evaluate_xpath(*document, "//p[contains(@class, 'blurb')]/text()");
    EXPECT_GE(text.nodes.size(), 1u);
    EXPECT_FALSE(text.string_values.empty());

    const auto heading = xpath_string_value(*document, "//h1");
    EXPECT_EQ(heading, "Hello");
}

TEST(XPath, InvalidExpressionThrows) {
    const auto document = parse_html("<html><body><p>x</p></body></html>");
    EXPECT_THROW(evaluate_xpath(*document, "//p["), Error);
    EXPECT_THROW(evaluate_xpath(*document, ""), Error);
}

TEST(XPath, EmptyDocumentIsInvalid) {
    const auto document = parse_html("<html></html>");
    EXPECT_NO_THROW(evaluate_xpath(*document, "count(//*)"));

    prowsetk::Document empty;
    EXPECT_THROW(evaluate_xpath(empty, "//*"), Error);
}

#endif  // PROWSETK_HAVE_PUGIXML

TEST(XPath, UnavailableEngineIsReported) {
#ifndef PROWSETK_HAVE_PUGIXML
    const auto document = parse_html("<html><body><p>x</p></body></html>");
    EXPECT_THROW(evaluate_xpath(*document, "//p"), Error);
#else
    GTEST_SKIP() << "pugixml is available in this build";
#endif
}