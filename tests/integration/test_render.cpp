#include <gtest/gtest.h>
#include <prowsetk/browser.hpp>
#include <prowsetk/render.hpp>

using namespace prowsetk;

TEST(RenderIntegration, HitTargetRunsSessionScriptAndRefreshesGeometry) {
    if (make_javascript_runtime()->name() == "null") GTEST_SKIP();
    Browser browser;
    auto transport = std::make_unique<MemoryNetworkClient>();
    transport->set_response("https://app.test/api/update", HttpResponse{200, {}, "{}", {}, {}});
    browser.set_network_client(std::move(transport));
    auto session = browser.create_session();
    session->load_html("<button id='button'>Update</button><div id='result' style='height:10px'>Old</div>"
        "<script>document.querySelector('#button').onclick=()=>{fetch('/api/update');"
        "Promise.resolve().then(()=>{let p=document.querySelector('#result');p.textContent='Updated';"
        "p.setAttribute('style','height:40px')})}</script>", "https://app.test/");
    auto page = render_document(*session->document());
    std::shared_ptr<Element> target;
    for (const auto& p : page.paint.items) {
        if (p.kind == PaintKind::Control && p.element->id() == "button")
            target = hit_test(page.paint, p.rect.x + 1, p.rect.y + 1).interactive;
    }
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->click(*session));
    EXPECT_EQ(session->document()->query_selector("#result")->text(), "Updated");
    ASSERT_EQ(session->page_script_requests().size(), 1u);
    page = render_document(*session->document());
    bool found = false;
    for (const auto& p : page.paint.items) {
        if (p.kind == PaintKind::Background && p.element->id() == "result") {
            EXPECT_DOUBLE_EQ(p.rect.height, 40); found = true;
        }
    }
    EXPECT_TRUE(found);
}
