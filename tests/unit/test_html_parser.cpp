#include <gtest/gtest.h>
#include "prowsetk/document.hpp"
#include "prowsetk/error.hpp"

using prowsetk::parse_html;

TEST(HtmlParser, RawTextRequiresDelimitedMatchingEndTag) {
    const auto doc = parse_html("<script>a</scripture>b</ScRiPt><p>end</p>"
                                "<style>x</style-sheet>y</STYLE>"
                                "<textarea>a</textarea-extra>&amp;<b>x</b></textarea>");
    EXPECT_EQ(doc->query_selector("script")->text(), "a</scripture>b");
    EXPECT_EQ(doc->query_selector("style")->text(), "x</style-sheet>y");
    EXPECT_EQ(doc->query_selector("textarea")->text(), "a</textarea-extra>&<b>x</b>");
    EXPECT_EQ(doc->query_selector("p")->text(), "end");
}

TEST(HtmlParser, NumericReferencesAreBoundedUnicodeScalars) {
    const auto doc = parse_html("<p>&#0;|&#xD800;|&#1114112;|&#999999999999999999999999999999;|"
                                "&#128;|&#x1f600;|&#65z|&#x41|&#-1;|&#x;</p>");
    EXPECT_EQ(doc->query_selector("p")->text(),
              "\xef\xbf\xbd|\xef\xbf\xbd|\xef\xbf\xbd|\xef\xbf\xbd|"
              "\xe2\x82\xac|\xf0\x9f\x98\x80|Az|A|&#-1;|&#x;");
}

TEST(HtmlParser, AttributesKeepFirstDuplicateAndDecodeValues) {
    const auto doc = parse_html("<input NAME='first' name=second :value='&#65;' @click='run()' data-url=/a/b>");
    const auto input = doc->query_selector("input");
    EXPECT_EQ(input->attribute("name"), "first");
    EXPECT_EQ(input->attribute(":value"), "A");
    EXPECT_EQ(input->attribute("@click"), "run()");
    EXPECT_EQ(input->attribute("data-url"), "/a/b");
    EXPECT_EQ(input->outer_html(), "<input name=\"first\" :value=\"A\" @click=\"run()\" data-url=\"/a/b\">");
}

TEST(HtmlParser, RecoversOmittedEndsThroughInlineDescendants) {
    const auto doc = parse_html("<ul><li><b>one<li>two</ul><p><em>intro<section>body</section>"
                                "<dl><dt><b>term<dd>definition</dl>"
                                "<table><tbody><tr><td><b>a<td>b<tr><td>c</table>"
                                "<select><optgroup><option>a<option>b<optgroup><option>c</select>");
    EXPECT_EQ(doc->query_selector_all("ul > li").size(), 2u);
    EXPECT_EQ(doc->query_selector("li")->text(), "one");
    EXPECT_EQ(doc->query_selector("p")->text(), "intro");
    EXPECT_EQ(doc->query_selector_all("p section").size(), 0u);
    EXPECT_EQ(doc->query_selector_all("dl > dd").size(), 1u);
    EXPECT_EQ(doc->query_selector_all("table > tbody > tr").size(), 2u);
    EXPECT_EQ(doc->query_selector_all("tr > td").size(), 3u);
    EXPECT_EQ(doc->query_selector_all("select > optgroup").size(), 2u);
    EXPECT_EQ(doc->query_selector_all("optgroup > option").size(), 3u);
}

TEST(HtmlParser, NestedListsKeepTheirOuterItemOpen) {
    const auto doc = parse_html("<ul><li>outer<ul><li><b>inner<li>next</ul><li>end</ul>");
    EXPECT_EQ(doc->query_selector_all("ul > li").size(), 4u);
    EXPECT_EQ(doc->query_selector_all("li > ul > li").size(), 2u);
    EXPECT_EQ(doc->query_selector_all("li > ul").size(), 1u);
}

TEST(HtmlParser, HtmlNonVoidSolidusDoesNotCloseElement) {
    const auto doc = parse_html("<div/>inside<br/>after</div><3 is text");
    EXPECT_EQ(doc->query_selector("div")->text(), "insideafter");
    EXPECT_NE(doc->text().find("<3 is text"), std::string::npos);
}

TEST(HtmlParser, SerializationPreservesRawAndEscapedText) {
    const auto doc = parse_html("<div data-x='&quot;&amp;&lt;'><script>a < b && c</script>"
                                "<textarea>&lt;b&gt;&amp;</textarea><!--note--></div>");
    const auto encoded = doc->query_selector("div")->outer_html();
    const auto copy = parse_html(encoded);
    EXPECT_EQ(copy->query_selector("div")->outer_html(), encoded);
    EXPECT_EQ(copy->query_selector("script")->text(), "a < b && c");
    EXPECT_EQ(copy->query_selector("textarea")->text(), "<b>&");
}

TEST(HtmlParser, UnterminatedRawTextAndAttributesRecoverAtEof) {
    EXPECT_EQ(parse_html("<style>a<b")->query_selector("style")->text(), "a<b");
    EXPECT_EQ(parse_html("<textarea>&amp;<b>")->query_selector("textarea")->text(), "&<b>");
    EXPECT_EQ(parse_html("<input value='tail")->query_selector("input")->attribute("value"), "tail");
}

TEST(HtmlParser, ResourceLimitsHaveExplicitErrorCodes) {
    const auto limited = [](const std::string& source) {
        try {
            (void)parse_html(source);
            ADD_FAILURE() << "expected a bounded parser failure";
        } catch (const prowsetk::Error& error) {
            EXPECT_EQ(error.code(), prowsetk::ErrorCode::ResourceLimit);
        }
    };
    limited(std::string(16 * 1024 * 1024 + 1, 'x'));
    std::string deep;
    for (int i = 0; i < 257; ++i) deep += "<div>";
    limited(deep);
    std::string wide;
    for (int i = 0; i < 250000; ++i) wide += "<br>";
    limited(wide);
    EXPECT_EQ(parse_html("<p>still usable</p>")->query_selector("p")->text(), "still usable");
}

TEST(HtmlParser, DepthBoundaryAndAmpersandRunsRemainUsable) {
    std::string source;
    for (int i = 0; i < 256; ++i) source += "<div>";
    for (int i = 0; i < 256; ++i) source += "</div>";
    const auto doc = parse_html(source);
    EXPECT_EQ(doc->query_selector_all("div").size(), 256u);
    EXPECT_EQ(doc->query_selector("div")->outer_html(), source);
    const std::string ampersands(100000, '&');
    EXPECT_EQ(parse_html("<p>" + ampersands + "</p>")->query_selector("p")->text(), ampersands);
}
