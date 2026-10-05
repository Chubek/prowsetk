#include <gtest/gtest.h>
#include <prowsetk/plugins/complex_gui.hpp>
#include <prowsetk/error.hpp>

using namespace prowsetk;
using namespace prowsetk::complex_gui;
namespace {
Target find_target(Controller& c, std::string_view id, PaintKind kind = PaintKind::Text) {
    for (const auto& p : c.page().paint.items) {
        if (p.kind == kind && p.element && p.element->id() == id) {
            auto t = c.target_at(p.rect.x + 1, p.rect.y + 1);
            if (t) return *t;
        }
    }
    ADD_FAILURE() << "target missing"; return {};
}
}
TEST(ComplexGui, OfflineHistoryAndViewportAreBoundedAndUsable) {
    BrowserConfig config; config.javascript = false;
    Browser browser(config); auto session = browser.create_session(); Controller c(*session);
    c.load_html("<h1>First</h1>","https://site.test/first?token=private");
    c.load_html("<h1>Second</h1>","https://site.test/second");
    EXPECT_TRUE(c.can_back()); c.back(); EXPECT_EQ(session->document()->query_selector("h1")->text(),"First");
    EXPECT_EQ(c.address(),"https://site.test/first");
    c.forward(); EXPECT_EQ(session->document()->query_selector("h1")->text(),"Second");
    c.reload(); c.resize(640,480);
    EXPECT_EQ(session->viewport().width,640);
    EXPECT_GE(c.page().content_width,640);
    EXPECT_THROW(c.resize(0,480),Error);
    EXPECT_THROW(c.navigate("file:///tmp/page.html"),Error);
    EXPECT_THROW(c.navigate("https://name:password@site.test/"),Error);
}
TEST(ComplexGui, NestedLinkUsesLiveAncestorAndRejectsStaleTargets) {
    BrowserConfig config; config.javascript = false; Browser browser(config);
    auto network = std::make_unique<MemoryNetworkClient>();
    network->set_response("https://site.test/next",HttpResponse{200,{},"<h1>Next</h1>",{}, {}});
    browser.set_network_client(std::move(network));
    auto session=browser.create_session(); Controller c(*session);
    c.load_html("<a href='/next'><b id='text'>Next</b></a>","https://site.test/");
    const auto old=find_target(c,"text");
    session->document()->query_selector("b")->set_text("Changed");
    EXPECT_FALSE(c.click(old));
    c.refresh(); const auto fresh=find_target(c,"text");
    ASSERT_TRUE(c.click(fresh));
    EXPECT_EQ(session->current_url(),"https://site.test/next");
    EXPECT_FALSE(c.click(fresh));
}
TEST(ComplexGui, FormValuesStayRedactedAndReadonlyRejectsEdits) {
    BrowserConfig config; config.javascript=false; Browser browser(config);
    auto session=browser.create_session(); Controller c(*session);
    c.load_html("<input id='field' value='old-private'><input id='locked' readonly><input id='hidden' type='checkbox' style='display:none'>");
    auto t=find_target(c,"field",PaintKind::Control);
    EXPECT_TRUE(c.edit(t,"new-private"));
    EXPECT_EQ(session->document()->query_selector("#field")->value(),"new-private");
    for (const auto& p:c.page().paint.items) {
        EXPECT_EQ(p.text.find("private"),std::string::npos);
        if (p.element) EXPECT_NE(p.element->id(),"hidden");
    }
    EXPECT_FALSE(c.edit(find_target(c,"locked",PaintKind::Control),"other"));
}
TEST(ComplexGui, ImageLoadingIsOptInAndReentryIsRejected) {
    BrowserConfig config; config.javascript=false; Browser browser(config);
    auto network=std::make_unique<MemoryNetworkClient>(); auto* observed=network.get();
    network->set_response("https://site.test/p.png",HttpResponse{200,{},"invalid-png",{}, {}});
    browser.set_network_client(std::move(network));
    auto session=browser.create_session(); Controller c(*session);
    c.load_html("<img src='/p.png' alt='image'>","https://site.test/");
    EXPECT_TRUE(observed->requests().empty());
    const auto sub=session->events().subscribe(EventType::BeforeRequest,[&](Event&) { EXPECT_THROW(c.refresh(),Error); });
    c.enable_images(true); c.refresh(); c.refresh();
    EXPECT_EQ(observed->requests().size(),1u);
    session->events().unsubscribe(sub);
}
TEST(ComplexGui, DisabledViewerIsExplicit) {
    if (available()) GTEST_SKIP();
    Browser browser; auto session=browser.create_session();
    EXPECT_THROW(Viewer viewer(*session),Error);
}

TEST(ComplexGui, ImageAttemptsAndHistoryHaveFiniteBudgets) {
    BrowserConfig config; config.javascript=false; Browser browser(config);
    auto network=std::make_unique<MemoryNetworkClient>(); auto* observed=network.get();
    browser.set_network_client(std::move(network));
    auto session=browser.create_session(); Controller c(*session);
    std::string html;
    for (int i=0;i<40;++i) html += "<img src='/" + std::to_string(i) + ".png' alt='missing'>";
    c.load_html(html,"https://images.test/"); c.enable_images(true); c.refresh();
    EXPECT_EQ(observed->requests().size(),32u); EXPECT_TRUE(c.page().limited);
    c.refresh(); EXPECT_EQ(observed->requests().size(),32u);
    c.enable_images(false);
    for (int i=0;i<35;++i) c.load_html("<p>history</p>","https://history.test/"+std::to_string(i));
    int steps=0;
    while (c.can_back() && steps<40) { c.back(); ++steps; }
    EXPECT_EQ(steps,31);
}
