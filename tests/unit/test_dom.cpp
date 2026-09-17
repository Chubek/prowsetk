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

TEST(Document, SerializesEscapedTextAndAttributes) {
    const auto document =
        parse_html("<div data-x=\"a &amp; &quot;b&quot;\">1 < 2 &amp; 3</div>"
                   "<img src=\"/x?a=1&amp;b=2\">");

    auto div = document->query_selector("div");
    ASSERT_NE(div, nullptr);
    div->set_attribute("title", "\"quoted\" & <tag>");

    const std::string html = document->html();
    EXPECT_NE(html.find("1 &lt; 2 &amp; 3"), std::string::npos);
    EXPECT_NE(html.find("title=\"&quot;quoted&quot; &amp; &lt;tag&gt;\""),
              std::string::npos);
    EXPECT_NE(html.find("<img src=\"/x?a=1&amp;b=2\">"), std::string::npos);
}

TEST(Document, KeepsRawTextSerializationForScriptsAndStyles) {
    const auto document =
        parse_html("<script>if (a < b && c > d) run();</script>");
    EXPECT_NE(document->html().find("if (a < b && c > d) run();"),
              std::string::npos);
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

TEST(Document, GetElementByIdDoesNotInterpretCssCharacters) {
    const auto document =
        parse_html("<div id=\"a.b\"></div><div id=\"c:d\"></div>");
    EXPECT_NE(document->get_element_by_id("a.b"), nullptr);
    EXPECT_NE(document->get_element_by_id("c:d"), nullptr);
    EXPECT_EQ(document->get_element_by_id("missing"), nullptr);
}

TEST(Document, GetElementsByTagNameIsCaseInsensitiveLiteral) {
    const auto document = parse_html("<DIV><SPAN></SPAN><span></span></DIV>");
    EXPECT_EQ(document->get_elements_by_tag_name("span").size(), 2u);
    EXPECT_EQ(document->get_elements_by_tag_name("SPAN").size(), 2u);
    EXPECT_EQ(document->get_elements_by_tag_name("div").size(), 1u);
    EXPECT_EQ(document->get_elements_by_tag_name("div ").size(), 0u);
}

TEST(Document, MissingLiteralIdDoesNotFallBackToSelectorSyntax) {
    const auto document = parse_html("<div id='a' class='b'></div><span id='a.b'></span>");
    ASSERT_NE(document->get_element_by_id("a"), nullptr);
    EXPECT_EQ(document->get_element_by_id("a")->tag_name(), "div");
    ASSERT_NE(document->get_element_by_id("a.b"), nullptr);
    EXPECT_EQ(document->get_element_by_id("a.b")->tag_name(), "span");
    EXPECT_EQ(document->get_element_by_id("a.b.missing"), nullptr);
    EXPECT_EQ(document->get_element_by_id("missing:unsupported"), nullptr);
    EXPECT_EQ(document->get_element_by_id("a, span"), nullptr);
}

TEST(Document, HandlesMalformedMarkup) {
    const auto document = parse_html("<div><p>unclosed<span>x");
    EXPECT_NE(document->query_selector("div"), nullptr);
    EXPECT_NE(document->query_selector("span"), nullptr);
    EXPECT_EQ(document->query_selector("span")->text(), "x");
}

TEST(Document, CreateElementAndAttach) {
    const auto document = parse_html("<div id='container'></div>", "http://x.test/");
    auto container = document->query_selector("#container");
    ASSERT_NE(container, nullptr);
    auto p = document->create_element("p");
    p->set_text("created");
    container->append_child(p);
    EXPECT_EQ(container->inner_html(), "<p>created</p>");
    EXPECT_NE(document->html().find("<p>created</p>"), std::string::npos);
}

TEST(Element, SiblingTraversal) {
    const auto document = parse_html(
        "<ul><li>A</li><li>B</li><li>C</li></ul>", "http://x.test/");
    auto first = document->query_selector("li");
    ASSERT_NE(first, nullptr);
    auto second = first->next_sibling();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->text(), "B");
    auto third = second->next_sibling();
    ASSERT_NE(third, nullptr);
    EXPECT_EQ(third->text(), "C");
    EXPECT_EQ(third->next_sibling(), nullptr);
    auto prev = third->previous_sibling();
    ASSERT_NE(prev, nullptr);
    EXPECT_EQ(prev->text(), "B");
    EXPECT_EQ(first->previous_sibling(), nullptr);
}

TEST(Element, ChildrenAndFirstChild) {
    const auto document = parse_html(
        "<div><p>1</p><span>2</span><p>3</p></div>", "http://x.test/");
    auto div = document->query_selector("div");
    ASSERT_NE(div, nullptr);
    auto children = div->children();
    EXPECT_EQ(children.size(), 3u);
    EXPECT_EQ(children[0]->tag_name(), "p");
    EXPECT_EQ(children[1]->tag_name(), "span");
    EXPECT_EQ(children[2]->tag_name(), "p");
    EXPECT_EQ(div->first_child()->tag_name(), "p");
    EXPECT_EQ(div->first_child()->text(), "1");
}

TEST(Element, RemoveChild) {
    const auto document = parse_html("<div><p id='a'>A</p><p id='b'>B</p></div>", "http://x.test/");
    auto div = document->query_selector("div");
    auto a = document->query_selector("#a");
    auto b = document->query_selector("#b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(div->remove_child(a));
    EXPECT_EQ(div->children().size(), 1u);
    EXPECT_EQ(div->children()[0]->text(), "B");
    EXPECT_FALSE(div->remove_child(a));
}

TEST(Element, SetTextReplacesChildren) {
    const auto document = parse_html("<div><p>old</p><span>old2</span></div>", "http://x.test/");
    auto div = document->query_selector("div");
    div->set_text("new content");
    EXPECT_EQ(div->text(), "new content");
    EXPECT_EQ(div->inner_html(), "new content");
    EXPECT_EQ(div->children().size(), 0u);
}

TEST(Element, AttributesRoundTrip) {
    const auto document = parse_html("<div id='test'></div>", "http://x.test/");
    auto div = document->query_selector("#test");
    ASSERT_NE(div, nullptr);
    div->set_attribute("data-value", "123");
    div->set_attribute("data-empty", "");
    EXPECT_TRUE(div->has_attribute("data-value"));
    EXPECT_EQ(div->attribute("data-value"), "123");
    EXPECT_TRUE(div->has_attribute("data-empty"));
    EXPECT_EQ(div->attribute("data-empty"), "");
    auto attrs = div->attributes();
    EXPECT_EQ(attrs.size(), 3u);
    EXPECT_TRUE(div->remove_attribute("data-value"));
    EXPECT_FALSE(div->has_attribute("data-value"));
    attrs = div->attributes();
    EXPECT_EQ(attrs.size(), 2u);
}

TEST(Element, SelectValueForInputTypes) {
    const auto document = parse_html(
        "<select><option value='a'>A</option><option value='b' selected>B</option></select>"
        "<select><option>first</option><option>second</option></select>"
        "<input type='checkbox' value='on' checked>"
        "<input type='radio' name='r' value='r1'>"
        "<input type='radio' name='r' value='r2' checked>", "http://x.test/");
    auto select1 = document->query_selector("select");
    EXPECT_EQ(select1->value(), "b");
    auto select2 = document->query_selector_all("select")[1];
    EXPECT_EQ(select2->value(), "first");
    auto checkbox = document->query_selector("input[type='checkbox']");
    EXPECT_EQ(checkbox->value(), "on");
    auto radio = document->query_selector("input[type='radio'][checked]");
    EXPECT_EQ(radio->value(), "r2");
}

TEST(Document, MetadataIncludesCharset) {
    const auto document = parse_html(
        "<html><head><meta charset='utf-8'></head></html>", "http://x.test/");
    const auto metadata = document->metadata();
    EXPECT_EQ(metadata.at("charset"), "utf-8");
}

TEST(Document, ResourceUrlsHandlesDataAttributes) {
    const auto document = parse_html(
        "<img data-src='lazy.jpg'><source data-srcset='video.mp4'>", "http://x.test/");
    const auto resources = document->resource_urls();
    EXPECT_TRUE(std::find(resources.begin(), resources.end(), "http://x.test/lazy.jpg") == resources.end());
}

TEST(Element, OuterHtmlIncludesSelf) {
    const auto document = parse_html("<div id='x'><p>inside</p></div>", "http://x.test/");
    auto div = document->query_selector("#x");
    ASSERT_NE(div, nullptr);
    const std::string outer = div->outer_html();
    EXPECT_NE(outer.find("<div id=\"x\">"), std::string::npos);
    EXPECT_NE(outer.find("<p>inside</p>"), std::string::npos);
    EXPECT_NE(outer.find("</div>"), std::string::npos);
}
