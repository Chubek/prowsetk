#include <gtest/gtest.h>
#include <prowsetk/plugins/complex_gui.hpp>
using namespace prowsetk;
using namespace prowsetk::complex_gui;

TEST(ComplexGuiIntegration, ClickAndEditDrainScriptNetworkAndRerender) {
    if (make_javascript_runtime()->name()=="null") GTEST_SKIP();
    Browser browser;
    auto network=std::make_unique<MemoryNetworkClient>(); auto* observed=network.get();
    network->set_response("https://app.test/api/save",HttpResponse{200,{},"{}",{}, {}});
    browser.set_network_client(std::move(network));
    auto session=browser.create_session(); Controller c(*session);
    c.load_html("<input id='input'><button id='button'>Save</button><p id='state'>Old</p>"
        "<script>document.querySelector('#input').oninput=()=>document.querySelector('#state').textContent='Edited';"
        "document.querySelector('#button').onclick=()=>{fetch('/api/save');Promise.resolve().then(()=>document.querySelector('#state').textContent='Saved')};</script>","https://app.test/");
    const auto target=[&](std::string_view id) {
        for (const auto& p:c.page().paint.items) if (p.kind==PaintKind::Control && p.element->id()==id)
            return c.target_at(p.rect.x+1,p.rect.y+1);
        return std::optional<Target>{};
    };
    auto input=target("input"); ASSERT_TRUE(input); EXPECT_TRUE(c.edit(*input,"typed-value"));
    EXPECT_EQ(session->document()->query_selector("#state")->text(),"Edited");
    auto button=target("button"); ASSERT_TRUE(button); EXPECT_TRUE(c.click(*button));
    EXPECT_EQ(session->document()->query_selector("#state")->text(),"Saved");
    EXPECT_EQ(observed->requests().size(),1u);
    bool rendered=false;
    for (const auto& p:c.page().paint.items) if (p.text=="Saved") rendered=true;
    EXPECT_TRUE(rendered);
}
TEST(ComplexGuiIntegration, NativeFacadeLoadingDoesNotOpenDisplayOrNetwork) {
    Browser browser; auto network=std::make_unique<MemoryNetworkClient>(); auto* observed=network.get();
    browser.set_network_client(std::move(network));
    browser.plugins().load_native(COMPLEX_GUI_PLUGIN_PATH); browser.plugins().initialize_all();
    EXPECT_TRUE(observed->requests().empty());
}
