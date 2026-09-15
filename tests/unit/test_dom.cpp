#include <gtest/gtest.h>

#include <algorithm>

#include "prowsetk/document.hpp"

using prowsetk::parse_html;

namespace {
const char* kHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <title>Example &amp; Demo</title>
  <meta name="description" content="a test page">
  <base href="https://example.com/base/">
  <link rel="stylesheet" href="site.css">
  <script src="app.js"></script>
</head>
<body>
  <h1 id="heading" class="title big">Hello</h1>
  <p>First paragraph</p>
  <p>Second <b>bold</b> paragraph</p>
  <a href="page.html">Relative</a>
  <a href="https://other.test/x">Absolute</a>
  <form action="/submit" method="POST">
    <input name="user" value="alice">
    <input type="hidden" name="token" value="secret">
  </form>
  <textarea name="notes">hello world</textarea>
  <script>var x = 1 < 2;</script>
</body>
</html>)HTML";
}

TEST(Document, ReadsTitleWithEntityDecoding) {
    const auto document = parse_html(kHtml, "https://example.com/");
    EXPECT_EQ(document->title(), "Example & Demo");
}

TEST(Document, QueriesElements) {
    const auto document = parse_html(kHtml, "https://example.com/");
    auto heading = document->query_selector("h1#heading.title");
    ASSERT_NE(heading, nullptr);
    EXPECT_EQ(heading->tag_name(), "h1");
    EXPECT_EQ(heading->text(), "Hello");
    EXPECT_EQ(heading->attribute("class"), "title big");

    const auto paragraphs = document->query_selector_all("p");
    ASSERT_EQ(paragraphs.size(), 2u);
    EXPECT_EQ(paragraphs[1]->text(), "Second bold paragraph");
}

TEST(Document, DiscoversLinksFormsAndScripts) {
    const auto document = parse_html(kHtml, "https://example.com/");
    EXPECT_EQ(document->links().size(), 2u);
    EXPECT_EQ(document->forms().size(), 1u);
    EXPECT_EQ(document->scripts().size(), 2u);
}

TEST(Document, ReadsMetadata) {
    const auto document = parse_html(kHtml, "https://example.com/");
    const auto metadata = document->metadata();
    EXPECT_EQ(metadata.at("title"), "Example & Demo");
    EXPECT_EQ(metadata.at("description"), "a test page");
    EXPECT_EQ(metadata.at("lang"), "en");
}

TEST(Document, BaseUrlResolvesResourceUrls) {
    const auto document = parse_html(kHtml, "https://example.com/");
    EXPECT_EQ(document->base_url(), "https://example.com/base/");
    const auto resources = document->resource_urls();
    ASSERT_FALSE(resources.empty());
    EXPECT_NE(std::find(resources.begin(), resources.end(),
                        "https://example.com/base/app.js"),
              resources.end());
    EXPECT_NE(std::find(resources.begin(), resources.end(),
                        "https://example.com/base/site.css"),
              resources.end());
}

TEST(Document, RawTextElementsAreNotParsedAsMarkup) {
    const auto document = parse_html(kHtml, "https://example.com/");
    const auto scripts = document->scripts();
    ASSERT_EQ(scripts.size(), 2u);
    EXPECT_NE(scripts[1]->text().find("x = 1 < 2"), std::string::npos);
}

TEST(Element, TraversalAndMutation) {
    const auto document = parse_html(kHtml, "https://example.com/");
    auto heading = document->get_element_by_id("heading");
    ASSERT_NE(heading, nullptr);

    auto body = heading->parent();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->tag_name(), "body");

    auto first = body->first_child();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->tag_name(), "h1");
    EXPECT_NE(first->next_sibling(), nullptr);

    heading->set_attribute("data-state", "ready");
    EXPECT_TRUE(heading->has_attribute("data-state"));
    EXPECT_EQ(heading->attribute("data-state"), "ready");
    EXPECT_TRUE(heading->remove_attribute("data-state"));
    EXPECT_FALSE(heading->has_attribute("data-state"));
}

TEST(Element, FormValuesAndSelectors) {
    const auto document = parse_html(kHtml, "https://example.com/");
    auto input = document->query_selector("input[name='user']");
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->value(), "alice");

    input->set_value("bob");
    EXPECT_EQ(input->value(), "bob");

    auto textarea = document->query_selector("textarea[name='notes']");
    ASSERT_NE(textarea, nullptr);
    EXPECT_EQ(textarea->value(), "hello world");

    auto form = document->query_selector("form");
    ASSERT_NE(form, nullptr);
    const auto controls = form->query_selector_all("input");
    EXPECT_EQ(controls.size(), 2u);
}

TEST(Element, MatchesSelector) {
    const auto document = parse_html(kHtml, "https://example.com/");
    auto heading = document->get_element_by_id("heading");
    ASSERT_NE(heading, nullptr);
    EXPECT_TRUE(heading->matches("h1.title"));
    EXPECT_FALSE(heading->matches("h2"));
}

TEST(Document, HandlesMalformedMarkup) {
    const auto document = parse_html("<div><p>unclosed<span>x");
    EXPECT_NE(document->query_selector("div"), nullptr);
    EXPECT_NE(document->query_selector("span"), nullptr);
    EXPECT_EQ(document->query_selector("span")->text(), "x");
}
