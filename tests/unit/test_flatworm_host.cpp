#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "prowsetk/document.hpp"
#include "prowsetk/flatworm_host.hpp"

using prowsetk::Document;
using prowsetk::ElementHandle;
using prowsetk::FlatwormScriptHost;
using prowsetk::HostRequest;
using prowsetk::HostResponse;
using prowsetk::kNoElement;
using prowsetk::make_detached_script_host;
using prowsetk::parse_html;

namespace {

std::shared_ptr<FlatwormScriptHost> detached_host(const std::string& html,
                                                  const std::string& url) {
    return make_detached_script_host(parse_html(html, url, url));
}

}  // namespace

TEST(FlatwormHost, QueriesInternStableHandles) {
    auto host = detached_host(
        "<html><body><p id='a' class='x'>first</p><p>second</p></body></html>",
        "https://example.test/");
    const auto first = host->query_selector_all("p");
    ASSERT_EQ(first.size(), 2u);
    const auto again = host->query_selector_all("p");
    EXPECT_EQ(first[0], again[0]);
    EXPECT_NE(first[0], first[1]);
    EXPECT_EQ(host->element_attribute(first[0], "id"), "a");
    EXPECT_TRUE(host->element_has_attribute(first[0], "class"));
    EXPECT_FALSE(host->element_has_attribute(first[1], "id"));
    EXPECT_EQ(host->element_text(first[0]), "first");
    EXPECT_EQ(host->element_tag_name(first[1]), "p");
    EXPECT_EQ(host->element_node_type(first[0]), 1);
}

TEST(FlatwormHost, ScopedQueriesAndMatching) {
    auto host = detached_host(
        "<html><body><div class='box'><span>hi</span></div></body></html>",
        "https://example.test/");
    const auto boxes = host->query_selector_all(".box");
    ASSERT_EQ(boxes.size(), 1u);
    const auto spans = host->query_selector_all_in(boxes[0], "span");
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_TRUE(host->element_matches(spans[0], "div > span"));
    EXPECT_FALSE(host->element_matches(spans[0], "p"));
    EXPECT_NE(host->element_parent(spans[0]), kNoElement);
    EXPECT_EQ(host->element_parent(kNoElement), kNoElement);
}

TEST(FlatwormHost, AttributeAndTextMutations) {
    auto host = detached_host("<html><body><p>x</p></body></html>",
                              "https://example.test/");
    const auto paragraph = host->query_selector_all("p")[0];
    host->element_set_attribute(paragraph, "data-k", "v");
    EXPECT_EQ(host->element_attribute(paragraph, "data-k"), "v");
    const auto attrs = host->element_attributes(paragraph);
    ASSERT_EQ(attrs.size(), 1u);
    EXPECT_EQ(attrs[0].first, "data-k");
    host->element_remove_attribute(paragraph, "data-k");
    EXPECT_FALSE(host->element_has_attribute(paragraph, "data-k"));
    host->element_set_text(paragraph, "rewritten");
    EXPECT_EQ(host->element_text(paragraph), "rewritten");
    EXPECT_EQ(host->element_inner_html(paragraph), "rewritten");
}

TEST(FlatwormHost, InnerHtmlParsesAndSerializes) {
    auto host = detached_host("<html><body><div></div></body></html>",
                              "https://example.test/");
    const auto div = host->query_selector_all("div")[0];
    host->element_set_inner_html(div, "<form><input name='q'></form>");
    EXPECT_EQ(host->query_selector_all("form").size(), 1u);
    EXPECT_EQ(host->query_selector_all("input[name='q']").size(), 1u);
    EXPECT_NE(host->element_inner_html(div).find("<form>"), std::string::npos);
    EXPECT_NE(host->element_outer_html(div).find("<div>"), std::string::npos);
    // Replacing content drops the old nodes from query results.
    host->element_set_inner_html(div, "<b>bold</b>");
    EXPECT_TRUE(host->query_selector_all("form").empty());
}

TEST(FlatwormHost, TreeMutationAndDetachedNodes) {
    auto host = detached_host(
        "<html><body><ul><li>a</li><li>c</li></ul></body></html>",
        "https://example.test/");
    const auto list = host->query_selector_all("ul")[0];
    const auto items = host->query_selector_all("li");
    const ElementHandle middle = host->create_element("li");
    host->element_set_text(middle, "b");
    ASSERT_TRUE(host->insert_before(list, middle, items[1]));
    const auto now = host->query_selector_all("li");
    ASSERT_EQ(now.size(), 3u);
    EXPECT_EQ(host->element_text(now[1]), "b");
    // append_child moves an attached node (re-parenting).
    ASSERT_TRUE(host->append_child(items[0], now[1]));
    const auto nested = host->query_selector_all("li");
    ASSERT_EQ(nested.size(), 3u);
    // Document order: "a" (containing "b"), then "c"; "b" now parents to "a".
    EXPECT_EQ(host->element_parent(nested[1]), items[0]);
    EXPECT_EQ(host->element_text(nested[1]), "b");
    ASSERT_TRUE(host->remove_child(items[0], nested[1]));
    EXPECT_EQ(host->query_selector_all("li").size(), 2u);
    EXPECT_TRUE(host->detach_element(host->query_selector_all("li")[0]));
    EXPECT_EQ(host->query_selector_all("li").size(), 1u);
}

TEST(FlatwormHost, TextAndCommentNodesAndFragmentParse) {
    auto host = detached_host("<html><body><p>t</p></body></html>",
                              "https://example.test/");
    const auto paragraph = host->query_selector_all("p")[0];
    const ElementHandle text = host->create_text_node("hello");
    EXPECT_EQ(host->element_node_type(text), 3);
    EXPECT_TRUE(host->append_child(paragraph, text));
    const auto children = host->element_child_nodes(paragraph);
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(host->element_text(children[1]), "hello");
    const ElementHandle comment = host->create_comment("note");
    EXPECT_EQ(host->element_node_type(comment), 8);
    auto fragment = host->parse_html_fragment("<em>one</em><strong>two</strong>");
    ASSERT_EQ(fragment.size(), 2u);
    EXPECT_EQ(host->element_tag_name(fragment[1]), "strong");
}

TEST(FlatwormHost, HostRequestUsesHookAndReportsErrors) {
    auto host = detached_host("<html><body></body></html>",
                              "https://example.test/");
    HostRequest probe;
    probe.method = "POST";
    probe.url = "https://example.test/api";
    probe.body = "x=1";
    HostResponse missing = host->host_request(probe);
    EXPECT_FALSE(missing.ok);
    EXPECT_FALSE(missing.error.empty());

    host->set_request_hook([](const HostRequest& request) {
        HostResponse response;
        response.ok = true;
        response.status = 201;
        response.body = request.method + " " + request.url + " " + request.body;
        response.final_url = request.url;
        response.headers.emplace_back("Content-Type", "text/plain");
        return response;
    });
    const HostResponse created = host->host_request(probe);
    ASSERT_TRUE(created.ok);
    EXPECT_EQ(created.status, 201);
    EXPECT_EQ(created.body, "POST https://example.test/api x=1");
    ASSERT_EQ(created.headers.size(), 1u);
    EXPECT_EQ(created.headers[0].first, "Content-Type");
}

TEST(FlatwormHost, RecordsResolvedPendingNavigation) {
    auto host = detached_host("<html><body></body></html>",
                              "https://example.test/page");
    prowsetk::PendingNavigation probe;
    EXPECT_FALSE(host->consume_pending_navigation(probe));
    host->request_navigation("/other?a=1");
    prowsetk::PendingNavigation navigation;
    ASSERT_TRUE(host->consume_pending_navigation(navigation));
    EXPECT_EQ(navigation.url, "https://example.test/other?a=1");
    EXPECT_EQ(navigation.method, "GET");
    // Consuming clears the request.
    EXPECT_FALSE(host->consume_pending_navigation(navigation));
    host->request_form_submission("/submit", "POST", "a=1");
    ASSERT_TRUE(host->consume_pending_navigation(navigation));
    EXPECT_EQ(navigation.method, "POST");
    EXPECT_EQ(navigation.body, "a=1");
}

TEST(FlatwormHost, DetachedFallbackStorageAndCookies) {
    auto host = detached_host("<html><body></body></html>",
                              "https://example.test/");
    host->set_storage_item("local", "theme", "dark");
    EXPECT_EQ(host->storage_item("local", "theme"), "dark");
    EXPECT_EQ(host->storage_keys("local"), std::vector<std::string>{"theme"});
    host->remove_storage_item("local", "theme");
    EXPECT_TRUE(host->storage_keys("local").empty());
    host->set_storage_item("session", "k", "v");
    host->clear_storage("session");
    EXPECT_TRUE(host->storage_keys("session").empty());
    EXPECT_EQ(host->cookie_header(), "");  // no cookie hook wired

    host->set_cookie_hooks([] { return std::string("a=1"); },
                           [](std::string_view) {});
    EXPECT_EQ(host->cookie_header(), "a=1");
}

TEST(FlatwormHost, PageMetadataAndTitle) {
    auto host = detached_host(
        "<html><head><title>Docs</title></head><body></body></html>",
        "https://example.test/");
    auto info = host->page_info();
    EXPECT_EQ(info.url, "https://example.test/");
    EXPECT_EQ(info.title, "Docs");
    host->set_document_title("Renamed");
    EXPECT_EQ(host->page_info().title, "Renamed");
    host->set_referrer("https://example.test/from");
    EXPECT_EQ(host->page_info().referrer, "https://example.test/from");
    auto navigator = host->navigator_info();
    EXPECT_FALSE(navigator.user_agent.empty());
    EXPECT_TRUE(navigator.cookie_enabled);
}

TEST(FlatwormHost, InstallReplacesDocumentAndInvalidatesRegistry) {
    auto host = detached_host("<html><body><p>one</p></body></html>",
                              "https://example.test/");
    const auto first = host->query_selector_all("p");
    ASSERT_EQ(first.size(), 1u);
    host->install(parse_html("<html><body><p>two</p><p>three</p></html>",
                             "https://example.test/b", "https://example.test/b"),
                  "https://example.test/b");
    EXPECT_EQ(host->query_selector_all("p").size(), 2u);
    // Stale handles resolve to empty results, never crash.
    EXPECT_EQ(host->element_text(first[0]), "");
    EXPECT_EQ(host->element_node_type(first[0]), 0);
}

TEST(FlatwormHost, MutationOperationsEmitDomEvents) {
    auto document = parse_html("<html><body><p>x</p></body></html>",
                               "https://example.test/", "https://example.test/");
    int mutations = 0;
    std::string last_kind;
    document->set_mutation_listener(
        [&](const prowsetk::flatworm::MutationInfo& info) {
            ++mutations;
            last_kind = info.kind;
        });
    auto host = make_detached_script_host(document);
    const auto paragraph = host->query_selector_all("p")[0];
    host->element_set_attribute(paragraph, "data-a", "1");
    EXPECT_GE(mutations, 1);
    EXPECT_EQ(last_kind, "attribute-set");
    const ElementHandle created = host->create_element("span");
    host->append_child(paragraph, created);
    EXPECT_EQ(last_kind, "child-added");
    EXPECT_GT(mutations, 1);
}
