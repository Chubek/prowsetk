#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "prowsetk/document.hpp"
#include "prowsetk/ir.hpp"
#include "prowsetk/xpath.hpp"

using prowsetk::Document;
using prowsetk::ProwseXasEvent;
using prowsetk::decode_prowse_vtd;
using prowsetk::emit_prowse_dom;
using prowsetk::emit_prowse_iml;
using prowsetk::emit_prowse_vtd;
using prowsetk::emit_prowse_xas;
using prowsetk::expand_prowse_iml;
using prowsetk::filter_prowse_xas;
using prowsetk::parse_html;

namespace {

std::shared_ptr<Document> sample_document() {
    return parse_html(R"HTML(
        <html lang="en">
          <head><title>IR</title></head>
          <body>
            <ul id="list">
              <li class="a">one</li>
              <li class="b">two<span data-x="1">!</span></li>
            </ul>
            <a href="/go?token=abc">Go</a>
          </body>
        </html>
    )HTML",
                      "https://example.test/");
}

std::shared_ptr<Document> escaped_text_document() {
    auto doc = parse_html("<html><body><p id='x'></p></body></html>");
    auto p = doc->query_selector("p#x");
    p->set_text("line1\n\"quoted\" \\\\ slash");
    return doc;
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t pos) {
    return static_cast<std::uint32_t>(bytes[pos]) |
           (static_cast<std::uint32_t>(bytes[pos + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[pos + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[pos + 3]) << 24u);
}

struct Token {
    std::uint8_t type = 0;
    std::uint32_t depth = 0;
    std::string path;
    std::string name;
    std::string value;
};

std::vector<Token> decode_vtd(const std::vector<std::uint8_t>& bytes) {
    std::vector<Token> tokens;
    if (bytes.size() < 9) {
        return tokens;
    }
    std::size_t pos = 9;  // "PVTD1" + event count
    while (pos < bytes.size()) {
        if (pos + 1 + 4 > bytes.size()) {
            break;
        }
        Token token;
        token.type = bytes[pos++];
        token.depth = read_u32(bytes, pos);
        pos += 4;
        const auto read_string = [&](std::string& out) {
            if (pos + 4 > bytes.size()) {
                return false;
            }
            const std::uint32_t len = read_u32(bytes, pos);
            pos += 4;
            if (pos + len > bytes.size()) {
                return false;
            }
            out.assign(reinterpret_cast<const char*>(bytes.data() + pos), len);
            pos += len;
            return true;
        };
        if (!read_string(token.path) || !read_string(token.name) ||
            !read_string(token.value)) {
            break;
        }
        tokens.push_back(std::move(token));
    }
    return tokens;
}

bool xpath_available(const Document& doc) {
    try {
        (void)prowsetk::evaluate_xpath(doc, "//*");
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

TEST(IrEmitters, IR01_XasEmptyForDefaultDocument) {
    Document empty;
    EXPECT_TRUE(emit_prowse_xas(empty).empty());
}

TEST(IrEmitters, IR02_XasHasRootStartAndEnd) {
    const auto events = emit_prowse_xas(*sample_document());
    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ(events.front().kind, "start");
    EXPECT_EQ(events.back().kind, "end");
}

TEST(IrEmitters, IR03_XasIncludesAttributeEvents) {
    const auto events = emit_prowse_xas(*sample_document());
    bool seen = false;
    for (const auto& event : events) {
        if (event.kind == "attribute" && event.name == "href") {
            seen = true;
            break;
        }
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR04_XasIncludesTextEvents) {
    const auto events = emit_prowse_xas(*sample_document());
    bool seen = false;
    for (const auto& event : events) {
        if (event.kind == "text" && event.value.find("Go") != std::string::npos) {
            seen = true;
            break;
        }
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR05_XasSkipsWhitespaceOnlyText) {
    const auto events = emit_prowse_xas(*sample_document());
    for (const auto& event : events) {
        if (event.kind != "text") {
            continue;
        }
        EXPECT_FALSE(event.value == " " || event.value == "\n" || event.value == "\t");
    }
}

TEST(IrEmitters, IR06_XasStableSiblingIndexes) {
    const auto events = emit_prowse_xas(*sample_document());
    bool seen_first = false;
    bool seen_second = false;
    for (const auto& event : events) {
        seen_first = seen_first || event.xpath.find("/li[1]") != std::string::npos;
        seen_second = seen_second || event.xpath.find("/li[2]") != std::string::npos;
    }
    EXPECT_TRUE(seen_first);
    EXPECT_TRUE(seen_second);
}

TEST(IrEmitters, IR07_XasDepthIncreasesForNestedElements) {
    const auto events = emit_prowse_xas(*sample_document());
    std::size_t max_depth = 0;
    for (const auto& event : events) {
        max_depth = std::max(max_depth, event.depth);
    }
    EXPECT_GE(max_depth, 3u);
}

TEST(IrEmitters, IR08_XasAttributeCarriesNameAndValue) {
    const auto events = emit_prowse_xas(*sample_document());
    bool ok = false;
    for (const auto& event : events) {
        if (event.kind == "attribute" && event.name == "id" && event.value == "list") {
            ok = true;
            break;
        }
    }
    EXPECT_TRUE(ok);
}

TEST(IrEmitters, IR09_XasTextPathHasTextFunction) {
    const auto events = emit_prowse_xas(*sample_document());
    bool ok = false;
    for (const auto& event : events) {
        if (event.kind == "text" && event.xpath.find("text()[") != std::string::npos) {
            ok = true;
            break;
        }
    }
    EXPECT_TRUE(ok);
}

TEST(IrEmitters, IR10_XasDeterministicAcrossCalls) {
    const auto a = emit_prowse_xas(*sample_document());
    const auto b = emit_prowse_xas(*sample_document());
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].kind, b[i].kind);
        EXPECT_EQ(a[i].xpath, b[i].xpath);
        EXPECT_EQ(a[i].value, b[i].value);
    }
}

TEST(IrEmitters, IR11_XasIncludesMultipleAttributes) {
    const auto events = emit_prowse_xas(*sample_document());
    std::size_t count = 0;
    for (const auto& event : events) {
        if (event.kind == "attribute") {
            ++count;
        }
    }
    EXPECT_GE(count, 4u);
}

TEST(IrEmitters, IR12_XasIncludesNestedTagName) {
    const auto events = emit_prowse_xas(*sample_document());
    bool span_seen = false;
    for (const auto& event : events) {
        if (event.tag == "span") {
            span_seen = true;
            break;
        }
    }
    EXPECT_TRUE(span_seen);
}

TEST(IrEmitters, IR13_XasStartsBeforeEnds) {
    const auto events = emit_prowse_xas(*sample_document());
    std::size_t starts = 0;
    std::size_t ends = 0;
    for (const auto& event : events) {
        if (event.kind == "start") {
            ++starts;
        }
        if (event.kind == "end") {
            ++ends;
        }
    }
    EXPECT_EQ(starts, ends);
}

TEST(IrEmitters, IR14_XasContainsAnchorPath) {
    const auto events = emit_prowse_xas(*sample_document());
    bool ok = false;
    for (const auto& event : events) {
        if (event.xpath.find("/a[1]") != std::string::npos) {
            ok = true;
            break;
        }
    }
    EXPECT_TRUE(ok);
}

TEST(IrEmitters, IR15_XasFilterHandlesInvalidXPath) {
    const auto filtered = filter_prowse_xas(*sample_document(), "//*[" /* broken */);
    EXPECT_TRUE(filtered.empty());
}

TEST(IrEmitters, IR16_DomEmptyForDefaultDocument) {
    Document empty;
    EXPECT_TRUE(emit_prowse_dom(empty).empty());
}

TEST(IrEmitters, IR17_DomContainsHtmlNode) {
    const auto nodes = emit_prowse_dom(*sample_document());
    ASSERT_FALSE(nodes.empty());
    EXPECT_EQ(nodes.front().tag, "html");
}

TEST(IrEmitters, IR18_DomContainsBodyNode) {
    const auto nodes = emit_prowse_dom(*sample_document());
    bool seen = false;
    for (const auto& node : nodes) {
        seen = seen || node.tag == "body";
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR19_DomIncludesAttributes) {
    const auto nodes = emit_prowse_dom(*sample_document());
    bool seen = false;
    for (const auto& node : nodes) {
        for (const auto& attribute : node.attributes) {
            if (attribute.name == "lang" && attribute.value == "en") {
                seen = true;
            }
        }
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR20_DomContainsAggregatedText) {
    const auto nodes = emit_prowse_dom(*sample_document());
    bool seen = false;
    for (const auto& node : nodes) {
        if (node.tag == "a" && node.text.find("Go") != std::string::npos) {
            seen = true;
        }
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR21_DomDepthStartsAtZero) {
    const auto nodes = emit_prowse_dom(*sample_document());
    ASSERT_FALSE(nodes.empty());
    EXPECT_EQ(nodes.front().depth, 0u);
}

TEST(IrEmitters, IR22_DomSiblingIndexesStable) {
    const auto nodes = emit_prowse_dom(*sample_document());
    bool first = false;
    bool second = false;
    for (const auto& node : nodes) {
        first = first || node.xpath.find("/li[1]") != std::string::npos;
        second = second || node.xpath.find("/li[2]") != std::string::npos;
    }
    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
}

TEST(IrEmitters, IR23_DomIsPreorderWalk) {
    const auto nodes = emit_prowse_dom(*sample_document());
    std::size_t ul_index = nodes.size();
    std::size_t li_index = nodes.size();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].tag == "ul" && ul_index == nodes.size()) {
            ul_index = i;
        }
        if (nodes[i].tag == "li" && li_index == nodes.size()) {
            li_index = i;
        }
    }
    ASSERT_NE(ul_index, nodes.size());
    ASSERT_NE(li_index, nodes.size());
    EXPECT_LT(ul_index, li_index);
}

TEST(IrEmitters, IR24_DomContainsAnchorPath) {
    const auto nodes = emit_prowse_dom(*sample_document());
    bool seen = false;
    for (const auto& node : nodes) {
        if (node.tag == "a" && node.xpath.find("/a[1]") != std::string::npos) {
            seen = true;
        }
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR25_DomTagsAreLowercase) {
    const auto nodes = emit_prowse_dom(*sample_document());
    for (const auto& node : nodes) {
        for (const char c : node.tag) {
            EXPECT_FALSE(c >= 'A' && c <= 'Z');
        }
    }
}

TEST(IrEmitters, IR26_DomXpathsUnique) {
    const auto nodes = emit_prowse_dom(*sample_document());
    std::vector<std::string> paths;
    for (const auto& node : nodes) {
        paths.push_back(node.xpath);
    }
    std::sort(paths.begin(), paths.end());
    auto it = std::adjacent_find(paths.begin(), paths.end());
    EXPECT_TRUE(it == paths.end());
}

TEST(IrEmitters, IR27_DomDeterministicAcrossCalls) {
    const auto a = emit_prowse_dom(*sample_document());
    const auto b = emit_prowse_dom(*sample_document());
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].xpath, b[i].xpath);
        EXPECT_EQ(a[i].tag, b[i].tag);
        EXPECT_EQ(a[i].text, b[i].text);
    }
}

TEST(IrEmitters, IR28_VtdHasMagicHeader) {
    const auto bytes = emit_prowse_vtd(*sample_document());
    ASSERT_GE(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 'P');
    EXPECT_EQ(bytes[1], 'V');
    EXPECT_EQ(bytes[2], 'T');
    EXPECT_EQ(bytes[3], 'D');
    EXPECT_EQ(bytes[4], '1');
}

TEST(IrEmitters, IR29_VtdStoresEventCount) {
    const auto bytes = emit_prowse_vtd(*sample_document());
    const auto events = emit_prowse_xas(*sample_document());
    ASSERT_GE(bytes.size(), 9u);
    EXPECT_EQ(read_u32(bytes, 5), events.size());
}

TEST(IrEmitters, IR30_VtdTokenCountMatchesEvents) {
    const auto bytes = emit_prowse_vtd(*sample_document());
    const auto tokens = decode_vtd(bytes);
    const auto events = emit_prowse_xas(*sample_document());
    EXPECT_EQ(tokens.size(), events.size());
}

TEST(IrEmitters, IR31_VtdContainsStartToken) {
    const auto tokens = decode_vtd(emit_prowse_vtd(*sample_document()));
    bool seen = false;
    for (const auto& token : tokens) {
        seen = seen || token.type == 1;
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR32_VtdContainsAttributeToken) {
    const auto tokens = decode_vtd(emit_prowse_vtd(*sample_document()));
    bool seen = false;
    for (const auto& token : tokens) {
        seen = seen || token.type == 2;
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR33_VtdContainsTextToken) {
    const auto tokens = decode_vtd(emit_prowse_vtd(*sample_document()));
    bool seen = false;
    for (const auto& token : tokens) {
        seen = seen || token.type == 3;
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR34_VtdContainsEndToken) {
    const auto tokens = decode_vtd(emit_prowse_vtd(*sample_document()));
    bool seen = false;
    for (const auto& token : tokens) {
        seen = seen || token.type == 4;
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR35_VtdEncodesDepth) {
    const auto tokens = decode_vtd(emit_prowse_vtd(*sample_document()));
    std::uint32_t max_depth = 0;
    for (const auto& token : tokens) {
        max_depth = std::max(max_depth, token.depth);
    }
    EXPECT_GE(max_depth, 2u);
}

TEST(IrEmitters, IR36_VtdEncodesPath) {
    const auto tokens = decode_vtd(emit_prowse_vtd(*sample_document()));
    bool seen = false;
    for (const auto& token : tokens) {
        if (token.path.find("/html[1]") != std::string::npos) {
            seen = true;
        }
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR37_VtdEncodesName) {
    const auto tokens = decode_vtd(emit_prowse_vtd(*sample_document()));
    bool seen = false;
    for (const auto& token : tokens) {
        if (token.name == "href" || token.name == "a") {
            seen = true;
        }
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR38_VtdEncodesValue) {
    const auto tokens = decode_vtd(emit_prowse_vtd(*sample_document()));
    bool seen = false;
    for (const auto& token : tokens) {
        if (token.value.find("/go") != std::string::npos ||
            token.value.find("Go") != std::string::npos) {
            seen = true;
        }
    }
    EXPECT_TRUE(seen);
}

TEST(IrEmitters, IR39_VtdDeterministicAcrossCalls) {
    const auto a = emit_prowse_vtd(*sample_document());
    const auto b = emit_prowse_vtd(*sample_document());
    EXPECT_EQ(a, b);
}

TEST(IrEmitters, IR40_ImlStartsWithDocumentForm) {
    const std::string iml = emit_prowse_iml(*sample_document());
    EXPECT_NE(iml.find("(document"), std::string::npos);
}

TEST(IrEmitters, IR41_ImlContainsElementForms) {
    const std::string iml = emit_prowse_iml(*sample_document());
    EXPECT_NE(iml.find("(element html"), std::string::npos);
    EXPECT_NE(iml.find("(element body"), std::string::npos);
}

TEST(IrEmitters, IR42_ImlIncludesAttributes) {
    const std::string iml = emit_prowse_iml(*sample_document());
    EXPECT_NE(iml.find("(lang \"en\")"), std::string::npos);
}

TEST(IrEmitters, IR43_ImlEscapesQuotes) {
    const std::string iml = emit_prowse_iml(*escaped_text_document());
    EXPECT_NE(iml.find("\\\"quoted\\\""), std::string::npos);
}

TEST(IrEmitters, IR44_ImlEscapesBackslash) {
    const std::string iml = emit_prowse_iml(*escaped_text_document());
    EXPECT_NE(iml.find("\\\\\\\\"), std::string::npos);
}

TEST(IrEmitters, IR45_ImlEscapesNewline) {
    const std::string iml = emit_prowse_iml(*escaped_text_document());
    EXPECT_NE(iml.find("line1\\n"), std::string::npos);
}

TEST(IrEmitters, IR46_ImlSkipsWhitespaceOnlyTextNodes) {
    const std::string iml = emit_prowse_iml(*sample_document());
    EXPECT_EQ(iml.find("(text \" \")"), std::string::npos);
}

TEST(IrEmitters, IR47_ImlContainsNestedElements) {
    const std::string iml = emit_prowse_iml(*sample_document());
    EXPECT_NE(iml.find("(element ul"), std::string::npos);
    EXPECT_NE(iml.find("(element li"), std::string::npos);
    EXPECT_NE(iml.find("(element span"), std::string::npos);
}

TEST(IrEmitters, IR48_ImlDeterministicAcrossCalls) {
    const auto a = emit_prowse_iml(*sample_document());
    const auto b = emit_prowse_iml(*sample_document());
    EXPECT_EQ(a, b);
}

TEST(IrEmitters, IR49_ImlEmptyForDefaultDocument) {
    Document empty;
    EXPECT_TRUE(emit_prowse_iml(empty).empty());
}

TEST(IrEmitters, IR50_XasFilterSelectsSubtreeWhenXPathAvailable) {
    const auto doc = sample_document();
    if (!xpath_available(*doc)) {
        GTEST_SKIP() << "XPath support unavailable in this build";
    }
    const auto filtered = filter_prowse_xas(*doc, "//ul[@id='list']");
    ASSERT_FALSE(filtered.empty());
    bool li_seen = false;
    for (const auto& event : filtered) {
        if (event.xpath.find("/li[") != std::string::npos) {
            li_seen = true;
            break;
        }
    }
    EXPECT_TRUE(li_seen);
}

TEST(IrEmitters, IR51_VtdDecodesToXasEvents) {
    const auto doc = sample_document();
    const auto decoded = decode_prowse_vtd(emit_prowse_vtd(*doc));
    const auto events = emit_prowse_xas(*doc);
    ASSERT_EQ(decoded.size(), events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(decoded[i].kind, events[i].kind);
        EXPECT_EQ(decoded[i].xpath, events[i].xpath);
        EXPECT_EQ(decoded[i].depth, events[i].depth);
    }
}

TEST(IrEmitters, IR52_VtdRejectsTruncatedInput) {
    auto bytes = emit_prowse_vtd(*sample_document());
    ASSERT_GT(bytes.size(), 10u);
    bytes.pop_back();
    EXPECT_TRUE(decode_prowse_vtd(bytes).empty());
}

TEST(IrEmitters, IR53_ImlExpandsKnownMacros) {
    const std::string iml =
        "(document\n  (macro upper \"go\" \"now\")\n)\n";
    const std::string expanded = expand_prowse_iml(
        iml, [](std::string_view name, const std::vector<std::string>& args) {
            if (name != "upper") {
                return std::string();
            }
            std::string out = "(text \"";
            for (const auto& arg : args) {
                std::string copy = arg;
                std::transform(copy.begin(), copy.end(), copy.begin(), [](char c) {
                    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                });
                out += copy;
            }
            out += "\")";
            return out;
        });
    EXPECT_NE(expanded.find("(text \"GONOW\")"), std::string::npos);
}

TEST(IrEmitters, IR54_ImlPreservesUnknownMacros) {
    const std::string iml = "(macro unknown \"x\")";
    const std::string expanded =
        expand_prowse_iml(iml, [](std::string_view, const std::vector<std::string>&) {
            return std::string();
        });
    EXPECT_EQ(expanded, iml);
}
