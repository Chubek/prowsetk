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

TEST(RenderIntegration, ResponsiveFlexSnapshotHitTestingAndScriptMutation) {
    if (make_javascript_runtime()->name() == "null") GTEST_SKIP();
    Browser browser;
    auto session = browser.create_session();
    session->load_html(R"(<style>
      #cards { display:flex; flex-wrap:wrap; gap:12px; }
      button { box-sizing:border-box; flex:0 0 140px; height:40px; }
    </style><div id='cards'><button id='first'>First</button><button id='second'>Second</button></div>
    <script>document.querySelector('#second').onclick=()=>{
      document.querySelector('#second').setAttribute('style','order:-1');
    };</script>)", "https://app.test/");
    const auto find = [](const RenderedPage& page, std::string_view id) -> const PaintItem* {
        for (const auto& p : page.paint.items)
            if (p.kind == PaintKind::Control && p.element->id() == id) return &p;
        return nullptr;
    };
    RenderOptions options;
    options.viewport_width = 320;
    auto page = render_document(*session->document(), options);
    auto first = find(page, "first"), second = find(page, "second");
    ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr);
    EXPECT_DOUBLE_EQ(first->rect.y, second->rect.y);
    EXPECT_GT(second->rect.x, first->rect.x);
    options.viewport_width = 200;
    page = render_document(*session->document(), options);
    first = find(page, "first"); second = find(page, "second");
    ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr);
    EXPECT_GT(second->rect.y, first->rect.bottom());
    const auto hit = hit_test(page.paint, second->rect.x + 2, second->rect.y + 2);
    ASSERT_NE(hit.interactive, nullptr);
    EXPECT_EQ(hit.interactive->id(), "second");
    ASSERT_TRUE(hit.interactive->click(*session));
    page = render_document(*session->document(), options);
    first = find(page, "first"); second = find(page, "second");
    ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr);
    EXPECT_LT(second->rect.y, first->rect.y);
    EXPECT_TRUE(session->page_script_requests().empty());
}
