#include <prowsetk/plugins/basic_gui.hpp>

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Help_View.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/Fl_Native_File_Chooser.H>

#include <prowsetk/error.hpp>
#include <prowsetk/lua_runtime.hpp>

namespace prowsetk::basic_gui {

struct Viewer::Impl {
    Controller controller;
    Fl_Double_Window window{1100, 800, "ProwseTk / Flatworm inspector"};
    Fl_Text_Buffer source_buffer, node_buffer, console_buffer, network_buffer, marionette_buffer, agent_reply_buffer;
    std::unique_ptr<Fl_Tabs> tabs;
    std::vector<std::unique_ptr<Fl_Group>> groups;
    std::unique_ptr<Fl_Button> back, forward, reload, open, go, find, click, type, evaluate, clear, load_lua, run_lua, connect_agent, ask_agent;
    std::unique_ptr<Fl_Input> address, selector, script, goal, server, agent_prompt;
    std::unique_ptr<Fl_Secret_Input> input;
    std::unique_ptr<Fl_Check_Button> values;
    std::unique_ptr<Fl_Help_View> page;
    std::unique_ptr<Fl_Hold_Browser> nodes;
    std::unique_ptr<Fl_Text_Display> source, node_detail, console, network, agent_reply;
    std::unique_ptr<Fl_Text_Editor> marionette_editor;
    std::unique_ptr<Fl_Box> status;
    std::optional<std::size_t> selected;
    struct Action { std::size_t node; std::uint64_t revision; };
    std::optional<Action> pending;
    std::uint64_t shown_revision = 0;
    int viewport_width = 0, viewport_height = 0;
    bool ticking = false;

    explicit Impl(Session& session) : controller(session) {
        window.begin();
        auto& nav = group(0, 0, 1100, 45);
        back = button(8, 8, 55, "Back");
        forward = button(67, 8, 65, "Forward");
        reload = button(136, 8, 65, "Reload");
        open = button(205, 8, 80, "Open HTML");
        address = std::make_unique<Fl_Input>(292, 8, 724, 28);
        address->when(FL_WHEN_ENTER_KEY_ALWAYS);
        address->callback(callback, this);
        go = button(1020, 8, 72, "Go");
        nav.resizable(address.get()); nav.end();

        auto& actions = group(0, 45, 1100, 45);
        selector = std::make_unique<Fl_Input>(90, 53, 280, 28, "CSS selector");
        find = button(375, 53, 60, "Inspect");
        click = button(440, 53, 60, "Click");
        input = std::make_unique<Fl_Secret_Input>(585, 53, 315, 28, "Type text");
        input->maximum_size(4096);
        type = button(905, 53, 125, "Type / append");
        actions.resizable(input.get()); actions.end();

        tabs = std::make_unique<Fl_Tabs>(8, 98, 1084, 597);
        auto& page_group = group(10, 125, 1080, 568, "Page");
        page = std::make_unique<Fl_Help_View>(14, 129, 1072, 560);
        page->user_data(this);
        page->link(link);
        page->textsize(16);
        page_group.resizable(page.get()); page_group.end();
        auto& dom_group = group(10, 125, 1080, 568, "DOM");
        nodes = std::make_unique<Fl_Hold_Browser>(14, 129, 525, 560);
        nodes->format_char(0);
        nodes->callback(callback, this);
        node_detail = display(545, 129, 541, 560, node_buffer);
        dom_group.resizable(node_detail.get()); dom_group.end();
        auto& source_group = group(10, 125, 1080, 568, "Source (sanitized)");
        source = display(14, 129, 1072, 560, source_buffer);
        source_group.resizable(source.get()); source_group.end();
        auto& console_group = group(10, 125, 1080, 568, "Console / events");
        console = display(14, 129, 1072, 560, console_buffer);
        console_group.resizable(console.get()); console_group.end();
        auto& network_group = group(10, 125, 1080, 568, "Network");
        network = display(14, 129, 1072, 560, network_buffer);
        network_group.resizable(network.get()); network_group.end();
        auto& agent_group = group(10, 125, 1080, 568, "OpenCode");
        server = std::make_unique<Fl_Input>(115, 137, 830, 28, "OpenCode URL");
        server->maximum_size(4096);
        server->tooltip("Empty uses OPENCODE_BASE_URL or loopback default. Authentication uses server environment variables.");
        connect_agent = button(960, 137, 120, "Check OpenCode");
        agent_prompt = std::make_unique<Fl_Input>(115, 177, 830, 28, "Ask about page");
        agent_prompt->maximum_size(4096);
        ask_agent = button(960, 177, 120, "Ask OpenCode");
        agent_reply = display(14, 217, 1072, 472, agent_reply_buffer);
        agent_reply_buffer.text("Check the OpenCode server, then ask about the displayed page structure.\n"
            "Replies are advisory; enable Console values to view subsequent replies.\n"
            "The Marionette tab runs permitted browser actions through opencode-marionette.");
        agent_group.resizable(agent_reply.get()); agent_group.end();
        auto& marionette_group = group(10, 125, 1080, 568, "Marionette");
        goal = std::make_unique<Fl_Input>(110, 137, 720, 28, "OpenCode goal");
        goal->maximum_size(4096);
        load_lua = button(840, 137, 110, "Load Lua");
        run_lua = button(960, 137, 120, "Run marionette");
        marionette_editor = std::make_unique<Fl_Text_Editor>(14, 177, 1072, 512);
        marionette_editor->buffer(marionette_buffer);
        marionette_editor->textfont(FL_COURIER); marionette_editor->textsize(13);
        marionette_buffer.text(
            "-- Load marionette.lua and edit its allowed actions for this page.\n"
            "-- OpenCode selects action IDs; the host executes them on this session.\n"
            "function main(args)\n"
            "  return [[{\"version\":1,\"goal\":\"Inspect the page\",\n"
            "    \"max_get_probes\":0,\"actions\":[]}]]\nend\n");
        if (!LuaRuntime::available()) run_lua->deactivate();
        marionette_group.resizable(marionette_editor.get()); marionette_group.end();
        tabs->resizable(&page_group); tabs->end();

        auto& eval_group = group(0, 700, 1100, 55);
        script = std::make_unique<Fl_Input>(90, 715, 675, 28, "JavaScript");
        script->maximum_size(65536);
        script->when(FL_WHEN_ENTER_KEY_ALWAYS); script->callback(callback, this);
        evaluate = button(770, 715, 80, "Evaluate");
        values = std::make_unique<Fl_Check_Button>(855, 715, 145, 28, "Console values");
        values->callback(callback, this);
        clear = button(1000, 715, 92, "Clear events");
        eval_group.resizable(script.get()); eval_group.end();
        status = std::make_unique<Fl_Box>(8, 765, 1084, 28);
        status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        window.resizable(tabs.get()); window.size_range(800, 600);
        window.callback(callback, this); window.end();
        controller.resize_viewport(page->w(), page->h());
        viewport_width = page->w(); viewport_height = page->h();
        refresh();
    }
    ~Impl() { stop(); }

    Fl_Group& group(int x, int y, int w, int h, const char* label = nullptr) {
        groups.push_back(std::make_unique<Fl_Group>(x, y, w, h, label));
        return *groups.back();
    }
    std::unique_ptr<Fl_Button> button(int x, int y, int width, const char* label) {
        auto widget = std::make_unique<Fl_Button>(x, y, width, 28, label);
        widget->callback(callback, this);
        return widget;
    }
    static std::unique_ptr<Fl_Text_Display> display(int x, int y, int w, int h, Fl_Text_Buffer& buffer) {
        auto widget = std::make_unique<Fl_Text_Display>(x, y, w, h);
        widget->buffer(buffer);
        widget->textfont(FL_COURIER); widget->textsize(13);
        return widget;
    }
    void message(const std::string& text) { status->copy_label(text.c_str()); }
    void safely(const std::function<void()>& action) noexcept {
        try { action(); }
        catch (const Error& error) { message(std::string("Operation failed: ") + to_string(error.code())); }
        catch (...) { message("Operation failed"); }
    }
    void select_node(std::size_t index) {
        const auto& snapshot = controller.snapshot();
        if (index >= snapshot.nodes.size()) return;
        selected = index;
        nodes->value(static_cast<int>(index + 1));
        nodes->make_visible(static_cast<int>(index + 1));
        const auto& node = snapshot.nodes[index];
        std::string text = node.path + "\n<" + node.tag + ">\n";
        if (node.hidden) text += "Hidden (semantic/inline heuristic)\n";
        if (node.disabled) text += "Disabled\n";
        for (const auto& attr : node.attributes) text += attr.name + " = " + attr.value + "\n";
        node_buffer.text(text.c_str());
    }
    std::optional<std::size_t> target() {
        if (selector->value()[0]) return controller.find(selector->value());
        return selected;
    }
    void render() {
        const auto& snapshot = controller.snapshot();
        if (shown_revision != snapshot.revision) {
            const auto top = page->topline();
            page->value(snapshot.preview_html.empty() ? "<h1>Flatworm inspector</h1><p>Enter an HTTP(S) URL or open an HTML file.</p>" : snapshot.preview_html.c_str());
            page->topline(top);
            source_buffer.text(snapshot.source_html.c_str());
            nodes->clear(); selected.reset(); node_buffer.text("");
            for (const auto& node : snapshot.nodes) {
                auto label = std::string(node.depth * 2, ' ') + "<" + node.tag + "> " + node.path;
                if (node.hidden) label += " [hidden]";
                if (node.disabled) label += " [disabled]";
                nodes->add(label.c_str());
            }
            address->value(snapshot.url.c_str());
            shown_revision = snapshot.revision;
            message(snapshot.limited ? "Page loaded; inspection snapshot limited" : "Flatworm basic preview | form values and scripts sanitized | no CSS layout");
        }
        if (controller.can_back()) back->activate(); else back->deactivate();
        if (controller.can_forward()) forward->activate(); else forward->deactivate();
        // The ring is a live view: navigation, request, redirect and cookie
        // activity belongs with the network tab; the rest is page activity.
        std::string console_text, network_text;
        if (controller.dropped_activity()) console_text = "[older activity discarded: " + std::to_string(controller.dropped_activity()) + "]\n";
        for (const auto& entry : controller.activity()) {
            const bool is_network = entry.type == "before_request" || entry.type == "after_response" ||
                                 entry.type == "before_redirect" || entry.type == "before_navigation" ||
                                 entry.type == "after_navigation" || entry.type == "cookie_change";
            std::string& text = is_network ? network_text : console_text;
            text += entry.type + " " + entry.detail + " " + entry.url + "\n";
        }
        console_buffer.text(console_text.c_str()); network_buffer.text(network_text.c_str());
    }
    void refresh() { controller.refresh(); render(); }
    void start() {
        window.show();
        if (!ticking) { ticking = true; Fl::add_timeout(0.1, tick, this); }
    }
    void stop() {
        ticking = false;
        Fl::remove_timeout(tick, this);
        pending.reset(); window.hide();
    }
    static void tick(void* data) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (!self.ticking) return;
        self.safely([&] {
            if (self.pending) {
                const auto action = *self.pending; self.pending.reset();
                const auto& snapshot = self.controller.snapshot();
                if (action.revision == snapshot.revision && action.node < snapshot.nodes.size()) {
                    const auto& tag = snapshot.nodes[action.node].tag;
                    if (tag == "textarea" || tag == "select" || tag == "input") {
                        self.tabs->value(self.groups[3].get()); // DOM tab
                        self.select_node(action.node);
                        self.selector->value("");
                        self.input->take_focus();
                        // Checkbox/radio clicks still execute their default action.
                        if (tag == "input") {
                            for (const auto& attr : snapshot.nodes[action.node].attributes) {
                                if (attr.name == "type" && (attr.value == "checkbox" || attr.value == "radio" || attr.value == "submit" || attr.value == "button")) {
                                    self.controller.click(action.node, action.revision); break;
                                }
                            }
                        }
                    } else self.controller.click(action.node, action.revision);
                }
            }
            const int width = self.page->w(), height = self.page->h();
            if (width != self.viewport_width || height != self.viewport_height) {
                self.controller.resize_viewport(width, height);
                self.viewport_width = width; self.viewport_height = height;
            }
            self.controller.pump(); self.render();
        });
        if (self.ticking) Fl::repeat_timeout(0.1, tick, data);
    }
    static const char* link(Fl_Widget* widget, const char* uri) noexcept {
        auto& self = *static_cast<Impl*>(widget->user_data());
        // Only synthetic IDs reach FLTK. Returning null prevents its file/URI
        // loader; activation is deferred until its link handler has returned.
        std::string_view text(uri ? uri : "");
        const auto prefix = text.rfind("prowse-action:");
        if (prefix == std::string_view::npos) return nullptr;
        text.remove_prefix(prefix + 14);
        const auto colon = text.find(':');
        if (colon == std::string_view::npos) return nullptr;
        std::uint64_t revision = 0; std::size_t node = 0;
        const auto first = std::from_chars(text.data(), text.data() + colon, revision);
        const auto second = std::from_chars(text.data() + colon + 1, text.data() + text.size(), node);
        if (first.ec == std::errc{} && first.ptr == text.data() + colon && second.ec == std::errc{} && second.ptr == text.data() + text.size()) {
            self.pending = Action{node, revision};
        }
        return nullptr;
    }
    static void callback(Fl_Widget* widget, void* data) noexcept {
        auto& self = *static_cast<Impl*>(data);
        self.safely([&] {
            if (widget == &self.window) { self.stop(); return; }
            if (widget == self.nodes.get()) {
                const auto index = self.nodes->value();
                if (index > 0) self.select_node(static_cast<std::size_t>(index - 1));
                return;
            }
            if (widget == self.values.get()) {
                self.controller.show_console_values(self.values->value() != 0); return;
            }
            if (widget == self.go.get() || widget == self.address.get()) {
                if (self.controller.snapshot().url == self.address->value()) self.controller.reload();
                else self.controller.navigate(self.address->value());
            }
            else if (widget == self.back.get()) self.controller.back();
            else if (widget == self.forward.get()) self.controller.forward();
            else if (widget == self.reload.get()) self.controller.reload();
            else if (widget == self.open.get()) {
                Fl_Native_File_Chooser chooser;
                chooser.title("Open HTML in Flatworm"); chooser.filter("HTML\t*.{html,htm}");
                if (chooser.show() != 0) return;
                const std::filesystem::path path(chooser.filename());
                const auto size = std::filesystem::file_size(path);
                if (size > 16u * 1024u * 1024u) throw Error(ErrorCode::ResourceLimit, "HTML file limit");
                std::ifstream file(path, std::ios::binary);
                std::string html(static_cast<std::size_t>(size), '\0');
                if (!file.read(html.data(), static_cast<std::streamsize>(html.size()))) throw Error(ErrorCode::InvalidArgument, "HTML file read failed");
                self.controller.load_html(html);
            } else if (widget == self.find.get()) {
                auto index = self.target();
                if (index) { self.tabs->value(self.groups[3].get()); self.select_node(*index); }
                else self.message("Selector has no inspection match");
                return;
            } else if (widget == self.click.get() || widget == self.type.get()) {
                auto index = self.target();
                const bool ok = index && (widget == self.click.get()
                    ? self.controller.click(*index, self.controller.snapshot().revision)
                    : self.controller.type(*index, self.controller.snapshot().revision, self.input->value()));
                self.input->value(""); self.render();
                self.message(ok ? "Interaction completed" : "Interaction unavailable, prevented, hidden, disabled or stale");
                return;
            } else if (widget == self.evaluate.get() || widget == self.script.get()) {
                const auto result = self.controller.evaluate(self.script->value());
                self.render(); self.message(result); return;
            } else if (widget == self.connect_agent.get() || widget == self.ask_agent.get()) {
                self.controller.set_opencode_base_url(self.server->value());
                self.message("Contacting OpenCode"); self.window.redraw(); Fl::flush();
                if (widget == self.connect_agent.get()) {
                    self.controller.check_opencode();
                    self.message("OpenCode API/authentication check succeeded");
                } else {
                    const auto reply = self.controller.ask_opencode(self.agent_prompt->value());
                    self.agent_reply_buffer.text(reply.c_str());
                    self.render(); self.message("OpenCode replied; advice is not executed");
                }
                return;
            } else if (widget == self.load_lua.get()) {
                Fl_Native_File_Chooser chooser;
                chooser.title("Load trusted Lua marionette"); chooser.filter("Lua\t*.lua");
                if (chooser.show() != 0) return;
                std::ifstream file(chooser.filename(), std::ios::binary);
                if (!file) throw Error(ErrorCode::IoError, "Lua file unavailable");
                std::string text(65537, '\0');
                file.read(text.data(), static_cast<std::streamsize>(text.size()));
                const auto size = static_cast<std::size_t>(file.gcount());
                if (size > 65536) throw Error(ErrorCode::ResourceLimit, "Lua file limit");
                if (file.bad() || (!file.eof() && file.fail())) throw Error(ErrorCode::IoError, "Lua read failed");
                text.resize(size);
                if (text.find('\0') != std::string::npos) throw Error(ErrorCode::InvalidArgument, "Lua contains NUL");
                self.marionette_buffer.text(text.c_str());
                self.message("Lua loaded; edit permitted actions, then Run marionette");
                return;
            } else if (widget == self.run_lua.get()) {
                self.controller.set_opencode_base_url(self.server->value());
                std::unique_ptr<char, decltype(&std::free)> text(self.marionette_buffer.text(), &std::free);
                self.message("Marionette running; waiting for OpenCode");
                self.window.redraw(); Fl::flush();
                try {
                    const auto result = self.controller.run_marionette(text.get(), self.goal->value());
                    self.render();
                    self.message("Marionette " + result.reason + "; actions: " + std::to_string(result.steps));
                } catch (...) { self.render(); throw; }
                return;
            } else if (widget == self.clear.get()) self.controller.clear_activity();
            self.render();
        });
    }
};

Viewer::Viewer(Session& session) : impl_(std::make_unique<Impl>(session)) {}
Viewer::~Viewer() = default;
void Viewer::show() { impl_->start(); }
void Viewer::navigate(std::string_view url) { impl_->controller.navigate(url); impl_->render(); }
void Viewer::load_html(std::string_view html, std::string_view base_url) { impl_->controller.load_html(html, base_url); impl_->render(); }
void Viewer::refresh() { impl_->refresh(); }
void Viewer::close() { impl_->stop(); }
void Viewer::set_opencode_base_url(std::string_view url) {
    impl_->controller.set_opencode_base_url(url);
    impl_->server->value(impl_->controller.opencode_base_url().c_str());
}
MarionetteResult Viewer::run_marionette(std::string_view lua, std::string_view goal) {
    impl_->message("Marionette running; waiting for OpenCode");
    impl_->window.redraw(); Fl::flush();
    try {
        const auto result = impl_->controller.run_marionette(lua, goal);
        impl_->render();
        impl_->message("Marionette " + result.reason + "; actions: " + std::to_string(result.steps));
        return result;
    } catch (...) { impl_->render(); throw; }
}
MarionetteResult Viewer::run_marionette_file(const std::filesystem::path& path, std::string_view goal) {
    impl_->message("Marionette running; waiting for OpenCode");
    impl_->window.redraw(); Fl::flush();
    try {
        const auto result = impl_->controller.run_marionette_file(path, goal);
        impl_->render();
        impl_->message("Marionette " + result.reason + "; actions: " + std::to_string(result.steps));
        return result;
    } catch (...) { impl_->render(); throw; }
}
int Viewer::exec() { show(); return Fl::run(); }
int run(Session& session) { Viewer viewer(session); return viewer.exec(); }

}  // namespace prowsetk::basic_gui
