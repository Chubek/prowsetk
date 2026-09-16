#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "prowsetk/document.hpp"

TEST(DomMutation, AppendRemoveAndTextMutation) {
    auto document =
        prowsetk::parse_html("<div id='a'></div>", "http://x.test/");
    auto container = document->query_selector("#a");
    ASSERT_NE(container, nullptr);

    auto paragraph = document->create_element("p");
    paragraph->set_text("hi");
    container->append_child(paragraph);
    EXPECT_EQ(container->children().size(), 1u);
    EXPECT_EQ(container->inner_html(), "<p>hi</p>");

    EXPECT_TRUE(container->remove_child(paragraph));
    EXPECT_EQ(container->inner_html(), "");

    container->set_text("replacement");
    EXPECT_EQ(container->text(), "replacement");
    EXPECT_EQ(container->inner_html(), "replacement");
}

TEST(DomMutation, ReportsMutationsThroughListener) {
    auto document =
        prowsetk::parse_html("<div id='a'></div>", "http://x.test/");
    std::vector<std::string> kinds;
    std::vector<std::string> node_names;
    document->set_mutation_listener(
        [&](const prowsetk::flatworm::MutationInfo& info) {
            kinds.push_back(info.kind);
            node_names.push_back(info.node_name);
        });

    auto container = document->query_selector("#a");
    auto paragraph = document->create_element("p");
    container->append_child(paragraph);
    container->set_attribute("data-role", "list");
    container->set_text("final");

    EXPECT_EQ(kinds, (std::vector<std::string>{"child-added",
                                               "attribute-set", "text-set"}));
    EXPECT_EQ(node_names,
              (std::vector<std::string>{"p", "div", "div"}));
}

TEST(DomMutation, ListenerClearedOnReset) {
    auto document =
        prowsetk::parse_html("<div id='a'></div>", "http://x.test/");
    int calls = 0;
    document->set_mutation_listener(
        [&](const prowsetk::flatworm::MutationInfo&) { ++calls; });
    document->query_selector("#a")->set_text("once");
    EXPECT_EQ(calls, 1);
    document->set_mutation_listener(nullptr);
    document->query_selector("#a")->set_text("silent");
    EXPECT_EQ(calls, 1);
}

TEST(DomMutation, SetValueOnTextareaReportsMutation) {
    auto document = prowsetk::parse_html(
        "<textarea id='t'></textarea>", "http://x.test/");
    int calls = 0;
    document->set_mutation_listener(
        [&](const prowsetk::flatworm::MutationInfo&) { ++calls; });
    auto textarea = document->query_selector("#t");
    textarea->set_value("typed");
    EXPECT_EQ(textarea->value(), "typed");
    EXPECT_EQ(calls, 1);
}