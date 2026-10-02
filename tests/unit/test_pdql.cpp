#include <gtest/gtest.h>
#include <prowsetk/browser.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/lua_runtime.hpp>
#include <prowsetk/pdql.h>
#include <prowsetk/pdql.hpp>

TEST(Pdql, LiveDomMixedContentGlobsAndProjections) {
    auto document = prowsetk::parse_html("<h1>Before <b>middle</b> after</h1><h2 data-n='4'>second</h2><p>other</p>");
    document->query_selector("h2")->set_text("changed");
    const prowsetk::pdql::Query query(R"(query Headings {
        from <h*>
        select { title: node.text.trim(), tag: node.tag_name }
        serialize as json
    })");
    const auto rows = query.execute(*document);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].fields.at("title").text, "Before middle after");
    EXPECT_EQ(rows[1].fields.at("title").text, "changed");
    EXPECT_EQ(document->query_selector("h1")->text(), "Before middle after");
}
TEST(Pdql, XPathUsesCoreEngineAndRejectsInvalidExpressions) {
#ifndef PROWSETK_HAVE_PUGIXML
    GTEST_SKIP() << "XPath is unavailable";
#endif
    const auto document = prowsetk::parse_html("<main><p class='price'>2</p><p class='price'>4</p></main><p>99</p>");
    const auto rows = prowsetk::pdql::Query("select text, count(), sum(), avg() from xpath(\"//main/p[contains(@class,'price')]\")").execute(*document);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].fields.at("count()").text, "2");
    EXPECT_DOUBLE_EQ(std::stod(rows[1].fields.at("sum()").text), 6);
    EXPECT_DOUBLE_EQ(std::stod(rows[1].fields.at("avg()").text), 3);
    EXPECT_THROW(prowsetk::pdql::Query("select text from xpath(\"//[\")"), prowsetk::Error);
    EXPECT_THROW(prowsetk::pdql::Query("select text from xpath(\"count(//p)\")").execute(*document), prowsetk::Error);
}
TEST(Pdql, DefaultRedactionPrecedesSelectionAndLeavesDomIntact) {
    const auto document = prowsetk::parse_html("<main><input value='private-input' data-token='private-token'><textarea>private-textarea</textarea><script>private-script</script><a href='/api?token=private-query&amp;page=2'>link</a></main>");
    const auto data = prowsetk::pdql::serialize(prowsetk::pdql::Query("select text, attr('value'), attr('data-token'), attr('href') from <*>").execute(*document), prowsetk::pdql::Format::Json);
    EXPECT_EQ(data.find("private-"), std::string::npos);
    EXPECT_NE(data.find("page=2"), std::string::npos);
    EXPECT_NE(document->html().find("private-input"), std::string::npos);
}
TEST(Pdql, SerializationEscapesControlsAndNames) {
    const std::vector<prowsetk::pdql::Row> rows{{{{"a<&", {"\t\r\n\"\\<&", false}}}}};
    const auto json = prowsetk::pdql::serialize(rows, prowsetk::pdql::Format::Json);
    EXPECT_NE(json.find("\\u0009"), std::string::npos);
    EXPECT_EQ(json.find('\t'), std::string::npos);
    const auto xml = prowsetk::pdql::serialize(rows, prowsetk::pdql::Format::Xml);
    EXPECT_NE(xml.find("name=\"a&lt;&amp;\""), std::string::npos);
    EXPECT_NE(prowsetk::pdql::serialize(rows, prowsetk::pdql::Format::Yaml).find("\"a<&\":"), std::string::npos);
    EXPECT_EQ(prowsetk::pdql::serialize({}, prowsetk::pdql::Format::Yaml), "[]\n");
    EXPECT_THROW(prowsetk::pdql::Query(std::string(65537, 'a')), prowsetk::Error);
    EXPECT_THROW(prowsetk::pdql::Query("select misspelled from <h1>"), prowsetk::Error);
    EXPECT_THROW(prowsetk::pdql::Query("<h1>").execute(prowsetk::Document{}), prowsetk::Error);
}
TEST(Pdql, CApiOwnershipAndErrorBoundary) {
    char* error = nullptr;
    auto* document = pt_pdql_document_create("<h1 id='one'>A</h1>", &error);
    ASSERT_NE(document, nullptr);
    ASSERT_EQ(error, nullptr);
    const auto* root = pt_pdql_document_root(document);
    ASSERT_EQ(pt_pdql_node_child_count(root), 1u);
    EXPECT_STREQ(pt_pdql_node_attribute(pt_pdql_node_child(root, 0), "ID"), "one");
    auto* query = pt_pdql_query_compile("select text from <h1>", &error);
    ASSERT_NE(query, nullptr);
    auto* result = pt_pdql_query_execute(query, document, &error);
    ASSERT_NE(result, nullptr);
    char* data = pt_pdql_result_serialize(result, PT_PDQL_JSON, &error);
    ASSERT_NE(data, nullptr);
    EXPECT_STREQ(data, "[{\"text\":\"A\"}]");
    pt_pdql_str_free(data); pt_pdql_result_free(result); pt_pdql_query_free(query); pt_pdql_document_free(document);
    EXPECT_EQ(pt_pdql_query_compile("select x from <p>", &error), nullptr);
    ASSERT_NE(error, nullptr); pt_pdql_str_free(error);
}
TEST(Pdql, LuaQueriesManagedDocumentsAndUrlHelpers) {
    if (!prowsetk::LuaRuntime::available()) GTEST_SKIP();
    prowsetk::BrowserConfig config;
    config.javascript = false;
    prowsetk::Browser browser(config);
    auto session = browser.create_session();
    session->load_html("<h1>before</h1>", "https://example.test/");
    prowsetk::LuaRuntime lua; lua.bind_browser(&browser); lua.bind_session(session);
    const auto result = lua.run(R"(
        local pdql = require('lpdql')
        session:document():query_selector('h1'):set_text('after')
        assert(pdql.query(session, 'select text from <h1>') == '[{"text":"after"}]')
        assert(pdql.rows(session:document(), 'select tag from <h1>')[1].tag == 'h1')
        assert(pdql.query(session, '<p>', 'yaml') == '[]\n')
        assert(not pcall(pdql.query, session, '<h1>', 'bad'))
        assert(not pcall(pdql.validate, 'select broken from <h1>'))
        local url = require('lprowse').url
        assert(url.resolve('https://example.test/a/b', '../c#x') == 'https://example.test/c#x')
        assert(url.normalize('https://EXAMPLE.test/a/../c#x') == 'https://example.test/c')
        assert(url.origin('https://example.test/c') == 'https://example.test')
        assert(not pcall(url.origin, 'https://user:pass@example.test/'))
        assert(not url.redact('https://example.test/?token=private'):find('private',1,true))
    )");
    EXPECT_TRUE(result.ok) << lua.last_error();
}
