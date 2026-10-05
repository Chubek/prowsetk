#include <prowsetk/plugins/complex_gui.hpp>
#include <prowsetk/error.hpp>
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>

namespace prowsetk::complex_gui {
namespace {
Fl_Font face(const FontSpec& font) {
    const bool mono = font.family.find("mono") != std::string::npos || font.family.find("courier") != std::string::npos;
    const auto base = mono ? FL_COURIER : FL_HELVETICA;
    return static_cast<Fl_Font>(base + (font.weight >= 600 ? 1 : 0) + (font.italic ? 2 : 0));
}
void use_font(const FontSpec& font) { fl_font(face(font), static_cast<int>(std::clamp(font.size, 1.0, 512.0))); }
class Metrics final : public TextMeasurer {
public:
    FontMetrics metrics(const FontSpec& font) const override {
        use_font(font);
        const auto height = static_cast<double>(fl_height());
        const auto descent = static_cast<double>(fl_descent());
        return {height - descent, descent, 0, height, fl_width(" "), fl_width("n")};
    }
    double measure(std::string_view text, const FontSpec& font) const override {
        use_font(font); return fl_width(text.data(), static_cast<int>(text.size()));
    }
};
Fl_Color color(Color c, double opacity = 1) {
    const double alpha = std::clamp(opacity * c.a / 255.0, 0.0, 1.0);
    const auto blend = [alpha](unsigned char channel) { return static_cast<unsigned char>(channel * alpha + 255 * (1 - alpha)); };
    return fl_rgb_color(blend(c.r), blend(c.g), blend(c.b));
}
bool editable(ControlKind c) {
    return c == ControlKind::TextField || c == ControlKind::PasswordField || c == ControlKind::TextArea;
}
}

struct Viewer::Impl {
    struct Canvas final : Fl_Widget {
        Impl& owner;
        Canvas(Impl& p, int x, int y, int w, int h) : Fl_Widget(x,y,w,h), owner(p) {}
        void draw() override { owner.paint(); }
        int handle(int event) override {
            if (event == FL_MOVE || event == FL_ENTER) {
                const auto target = owner.controller.target_at(Fl::event_x()-x()+owner.scroll_x(),Fl::event_y()-y()+owner.scroll_y());
                owner.window.cursor(target ? (editable(target->control) ? FL_CURSOR_INSERT : FL_CURSOR_HAND) : FL_CURSOR_DEFAULT);
                return 1;
            }
            if (event == FL_LEAVE) { owner.window.cursor(FL_CURSOR_DEFAULT); return 1; }
            if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) { take_focus(); return 1; }
            if (event == FL_RELEASE && Fl::event_button() == FL_LEFT_MOUSE) {
                owner.activate(Fl::event_x() - x() + owner.scroll_x(), Fl::event_y() - y() + owner.scroll_y());
                return 1;
            }
            if (event == FL_MOUSEWHEEL) {
                auto& bar = Fl::event_state(FL_SHIFT) ? *owner.horizontal : *owner.vertical;
                bar.value(static_cast<int>(std::clamp(bar.value() + Fl::event_dy() * 40.0, bar.minimum(), bar.maximum())));
                redraw(); return 1;
            }
            if (event == FL_FOCUS || event == FL_UNFOCUS) return 1;
            if (event == FL_KEYDOWN && (Fl::event_key() == FL_Page_Down || Fl::event_key() == FL_Page_Up)) {
                auto& bar = *owner.vertical;
                bar.value(static_cast<int>(std::clamp<double>(bar.value() + (Fl::event_key() == FL_Page_Down ? h() : -h()), bar.minimum(), bar.maximum())));
                redraw(); return 1;
            }
            return Fl_Widget::handle(event);
        }
    };
    Metrics metrics;
    Controller controller;
    Fl_Double_Window window{1100, 800, "ProwseTk / display-list browser"};
    std::unique_ptr<Fl_Button> back, forward, reload, open, go, apply, cancel;
    std::unique_ptr<Fl_Input> address;
    std::unique_ptr<Fl_Secret_Input> editor;
    std::unique_ptr<Fl_Check_Button> images;
    std::unique_ptr<Canvas> canvas;
    std::unique_ptr<Fl_Scrollbar> vertical, horizontal;
    std::unique_ptr<Fl_Box> status;
    std::optional<Target> editing;
    std::uint64_t shown_revision = 0;
    bool busy = false, scheduled = false;
    int viewport_w = 0, viewport_h = 0;

    explicit Impl(Session& session) : controller(session) {
        window.begin();
        back = button(8,8,55,"Back"); forward = button(67,8,65,"Forward");
        reload = button(136,8,65,"Reload"); open = button(205,8,85,"Open HTML");
        address = std::make_unique<Fl_Input>(298,8,608,28);
        address->maximum_size(8192); address->when(FL_WHEN_ENTER_KEY_ALWAYS); address->callback(callback,this);
        go = button(912,8,48,"Go");
        images = std::make_unique<Fl_Check_Button>(968,8,124,28,"Load images"); images->callback(callback,this);
        canvas = std::make_unique<Canvas>(*this,8,44,1068,676);
        vertical = std::make_unique<Fl_Scrollbar>(1076,44,16,676);
        horizontal = std::make_unique<Fl_Scrollbar>(8,720,1068,16); horizontal->type(FL_HORIZONTAL);
        vertical->callback(scroll_callback,this); horizontal->callback(scroll_callback,this);
        editor = std::make_unique<Fl_Secret_Input>(126,744,760,26,"Replace value:"); editor->maximum_size(4096);
        apply = button(894,744,92,"Apply value"); cancel = button(994,744,98,"Cancel");
        editor->deactivate(); apply->deactivate(); cancel->deactivate();
        status = std::make_unique<Fl_Box>(8,774,1084,22,"Ready — bounded CSS display-list preview");
        status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
        window.resizable(canvas.get()); window.size_range(640,400); window.end();
        controller.set_measurer(&metrics);
    }
    ~Impl() {
        if (scheduled) Fl::remove_timeout(tick,this);
        window.hide();
        // unique_ptr widgets detach themselves from their parent on destruction.
    }
    std::unique_ptr<Fl_Button> button(int x,int y,int w,const char* text) {
        auto b = std::make_unique<Fl_Button>(x,y,w,28,text); b->callback(callback,this); return b;
    }
    double scroll_x() const { return horizontal->value(); }
    double scroll_y() const { return vertical->value(); }
    void clear_editor() {
        editing.reset(); editor->value(""); editor->deactivate(); apply->deactivate(); cancel->deactivate();
    }
    void message(const char* text) { status->copy_label(text); status->redraw(); }
    void run(const std::function<void()>& action) {
        if (busy) return;
        busy = true;
        try { action(); sync(); }
        catch (const Error& e) { clear_editor(); message(to_string(e.code())); canvas->redraw(); }
        catch (...) { clear_editor(); message("Operation failed"); canvas->redraw(); }
        busy = false;
    }
    static void scroll_callback(Fl_Widget*,void* data) { static_cast<Impl*>(data)->canvas->redraw(); }
    static void callback(Fl_Widget* widget,void* data) {
        auto& p = *static_cast<Impl*>(data);
        p.run([&] {
            if (widget == p.go.get() || widget == p.address.get()) { p.clear_editor(); p.controller.navigate(p.address->value()); }
            else if (widget == p.back.get()) { p.clear_editor(); p.controller.back(); }
            else if (widget == p.forward.get()) { p.clear_editor(); p.controller.forward(); }
            else if (widget == p.reload.get()) { p.clear_editor(); p.controller.reload(); }
            else if (widget == p.images.get()) { p.controller.enable_images(p.images->value() != 0); p.controller.refresh(); }
            else if (widget == p.open.get()) p.open_file();
            else if (widget == p.cancel.get()) p.clear_editor();
            else if (widget == p.apply.get() && p.editing) {
                const auto target = *p.editing;
                std::string value = p.editor->value(); p.clear_editor();
                if (!p.controller.edit(target,value)) p.message("Control changed; select it again");
                std::fill(value.begin(),value.end(),'\0');
            }
        });
    }
    void open_file() {
        Fl_Native_File_Chooser chooser;
        chooser.title("Open local HTML"); chooser.type(Fl_Native_File_Chooser::BROWSE_FILE); chooser.filter("HTML\t*.{html,htm}");
        if (chooser.show() != 0 || !chooser.filename()) return;
        const std::filesystem::path path(chooser.filename());
        const auto size = std::filesystem::file_size(path);
        if (size > 16u*1024u*1024u) throw Error(ErrorCode::ResourceLimit,"HTML limit");
        std::ifstream stream(path,std::ios::binary);
        std::string html(static_cast<std::size_t>(size),'\0');
        if (!stream.read(html.data(),static_cast<std::streamsize>(html.size()))) throw Error(ErrorCode::InvalidArgument,"file read failed");
        clear_editor(); controller.load_html(html);
    }
    void activate(double x,double y) {
        run([&] {
            const auto target = controller.target_at(x,y); if (!target) return;
            if (editable(target->control)) {
                editing = target; editor->value(""); editor->activate(); apply->activate(); cancel->activate(); editor->take_focus();
                message("Enter replacement text, then Apply (values stay hidden)");
            } else { clear_editor(); controller.click(*target); }
        });
    }
    void sync() {
        if (shown_revision != controller.revision()) {
            if (editing && editing->revision != controller.revision()) clear_editor();
            shown_revision = controller.revision();
            address->value(controller.address().c_str());
            message(controller.page().limited ? "Partial page: render/image limit reached" : "Ready — bounded CSS display-list preview");
            canvas->redraw();
        }
        if (controller.can_back()) back->activate(); else back->deactivate();
        if (controller.can_forward()) forward->activate(); else forward->deactivate();
        const auto bar = [](Fl_Scrollbar& b,double content,int viewport) {
            const double maximum = std::max(0.0,content-viewport);
            b.bounds(0,maximum); b.slider_size(content > 0 ? static_cast<float>(std::min(1.0,viewport/content)) : 1.0f);
            b.value(static_cast<int>(std::clamp<double>(b.value(),0.0,maximum))); b.redraw();
        };
        bar(*vertical,controller.page().content_height,canvas->h());
        bar(*horizontal,controller.page().content_width,canvas->w());
    }
    static void tick(void* data) {
        auto& p = *static_cast<Impl*>(data); p.scheduled = false;
        if (!p.window.shown()) return;
        p.run([&] {
            if (p.canvas->w() != p.viewport_w || p.canvas->h() != p.viewport_h) {
                p.viewport_w = p.canvas->w(); p.viewport_h = p.canvas->h();
                p.controller.resize(p.viewport_w,p.viewport_h);
            } else p.controller.pump();
        });
        p.scheduled = true; Fl::repeat_timeout(0.1,tick,&p);
    }
    void paint() {
        fl_push_clip(canvas->x(),canvas->y(),canvas->w(),canvas->h());
        fl_color(FL_WHITE); fl_rectf(canvas->x(),canvas->y(),canvas->w(),canvas->h());
        const Rect viewport{scroll_x(),scroll_y(),static_cast<double>(canvas->w()),static_cast<double>(canvas->h())};
        for (const auto& p : controller.page().paint.items) {
            if (p.rect.intersect(viewport).empty() || p.opacity <= 0) continue;
            const auto visible = p.clip ? p.clip->intersect(viewport) : viewport;
            if (visible.empty()) continue;
            const auto dx = [this](double x) { return canvas->x() + static_cast<int>(std::clamp(x-scroll_x(),-1e8,1e8)); };
            const auto dy = [this](double y) { return canvas->y() + static_cast<int>(std::clamp(y-scroll_y(),-1e8,1e8)); };
            fl_push_clip(dx(visible.x),dy(visible.y),static_cast<int>(visible.width),static_cast<int>(visible.height));
            const int x=dx(p.rect.x), y=dy(p.rect.y), w=static_cast<int>(std::min(p.rect.width,1e8)), h=static_cast<int>(std::min(p.rect.height,1e8));
            fl_color(color(p.color,p.opacity));
            switch (p.kind) {
            case PaintKind::Background:
                if (p.color.a) fl_rectf(x,y,w,h);
                break;
            case PaintKind::Border: {
                const int style = p.border_style == BorderStyle::Dashed ? FL_DASH : p.border_style == BorderStyle::Dotted ? FL_DOT : FL_SOLID;
                const int edges[4][4] = {{x,y,x+w,y},{x+w,y,x+w,y+h},{x,y+h,x+w,y+h},{x,y,x,y+h}};
                for (int i=0;i<4;++i) if (p.border_width[i]>0) {
                    fl_line_style(style,static_cast<int>(p.border_width[i])); fl_line(edges[i][0],edges[i][1],edges[i][2],edges[i][3]);
                }
                fl_line_style(0); break;
            }
            case PaintKind::Text: case PaintKind::Marker:
                use_font(p.font); fl_draw(p.text.data(),static_cast<int>(p.text.size()),x,y+static_cast<int>(p.baseline)); break;
            case PaintKind::Control: {
                fl_color(p.disabled ? fl_rgb_color(235,235,235) : FL_WHITE); fl_rectf(x,y,w,h);
                fl_color(fl_rgb_color(90,90,90)); fl_rect(x,y,w,h);
                use_font(p.font);
                if (p.control == ControlKind::Checkbox || p.control == ControlKind::Radio) {
                    if (p.checked) { fl_line(x+2,y+2,x+w-2,y+h-2); fl_line(x+2,y+h-2,x+w-2,y+2); }
                } else {
                    fl_push_clip(x+2,y+1,std::max(0,w-4),std::max(0,h-2));
                    fl_draw(p.text.data(),static_cast<int>(p.text.size()),x+4,y+std::min(h-2,static_cast<int>(p.font.size)+2)); fl_pop_clip();
                }
                break;
            }
            case PaintKind::Image:
                if (p.bitmap && w>0 && h>0 && static_cast<double>(w)*h <= 4u*1024u*1024u) {
                    Fl_RGB_Image image(p.bitmap->rgba().data(),static_cast<int>(p.bitmap->width()),static_cast<int>(p.bitmap->height()),4);
                    double iw=w, ih=h;
                    if (p.object_fit != ObjectFit::Fill) {
                        const double ratio = p.object_fit == ObjectFit::Cover ? std::max(w/static_cast<double>(image.w()),h/static_cast<double>(image.h())) : std::min(w/static_cast<double>(image.w()),h/static_cast<double>(image.h()));
                        const double scale = p.object_fit == ObjectFit::None ? 1 : p.object_fit == ObjectFit::ScaleDown ? std::min(1.0,ratio) : ratio;
                        iw=image.w()*scale; ih=image.h()*scale;
                    }
                    if (iw*ih <= 4u*1024u*1024u) {
                        fl_push_clip(x,y,w,h); image.scale(std::max(1,static_cast<int>(iw)),std::max(1,static_cast<int>(ih)),0,1);
                        image.draw(x+(w-image.w())/2,y+(h-image.h())/2); fl_pop_clip();
                    }
                } else {
                    fl_color(fl_rgb_color(100,100,100)); fl_rect(x,y,w,h); fl_font(FL_HELVETICA,12);
                    fl_push_clip(x,y,w,h); fl_draw(p.alt.data(),static_cast<int>(p.alt.size()),x+2,y+14); fl_pop_clip();
                }
                break;
            }
            fl_pop_clip();
        }
        fl_pop_clip();
    }
};
bool available() noexcept { return true; }
Viewer::Viewer(Session& session) : impl_(std::make_unique<Impl>(session)) {}
Viewer::~Viewer() = default;
Controller& Viewer::controller() { return impl_->controller; }
void Viewer::show() {
    impl_->window.show();
    if (!impl_->scheduled) { impl_->scheduled=true; Fl::add_timeout(0,Impl::tick,impl_.get()); }
}
void Viewer::hide() {
    if (impl_->scheduled) { Fl::remove_timeout(Impl::tick,impl_.get()); impl_->scheduled=false; }
    impl_->window.hide();
}
int Viewer::exec() { show(); return Fl::run(); }
void Viewer::refresh() { impl_->run([this] { impl_->controller.refresh(); }); }
}
