#ifndef PROWSETK_PLUGINS_COMPLEX_GUI_HPP
#define PROWSETK_PLUGINS_COMPLEX_GUI_HPP
#include <prowsetk/browser.hpp>
#include <prowsetk/render.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace prowsetk::complex_gui {
struct Target {
    std::uint64_t revision = 0;
    std::size_t item = 0;
    ControlKind control = ControlKind::None;
};

// Display-free controller. Borrows Session and its Browser; both must outlive it.
// Calls are serialized on the host/UI thread; reentry fails with InvalidArgument.
// Page pixels/text are local presentation, not sanitized exports. Form values
// remain redacted; editing accepts a new value without reading it back to widgets.
class Controller {
public:
    explicit Controller(Session& session);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    void navigate(std::string_view url);
    void load_html(std::string_view html, std::string_view base = "https://offline.test/");
    void back();
    void forward();
    void reload();
    bool can_back() const noexcept;
    bool can_forward() const noexcept;
    void refresh();
    void pump();
    void resize(int width, int height);
    // Borrowed measurer must outlive this controller, or be reset to nullptr.
    void set_measurer(const TextMeasurer* measurer);
    // Explicit opt-in. At most 32 image loads per page and 8 MiB cache; all
    // requests use Session. No external stylesheets or toolkit URL loaders.
    void enable_images(bool enabled);
    bool images_enabled() const noexcept { return images_enabled_; }
    const RenderedPage& page() const noexcept { return page_; }
    std::uint64_t revision() const noexcept { return revision_; }
    std::optional<Target> target_at(double x, double y) const;
    bool click(Target target);
    bool edit(Target target, std::string_view replacement);
    std::string address() const; // no userinfo/query/fragment
private:
    struct Entry { std::string url; std::optional<std::string> html; };
    class Loader;
    void refresh_impl();
    void remember(std::optional<std::string> html = {});
    void travel(std::size_t index);
    std::shared_ptr<Element> resolve(Target target) const;
    Session& session_;
    SubscriptionId subscription_ = 0;
    std::unique_ptr<Loader> loader_;
    ImageCache images_;
    RenderOptions options_;
    RenderedPage page_;
    std::shared_ptr<Document> document_;
    std::vector<Entry> history_;
    std::size_t history_index_ = 0;
    std::uint64_t revision_ = 0, mutation_ = 0;
    bool dirty_ = true, active_ = false, images_enabled_ = false;
};

bool available() noexcept;
// Explicit FLTK window; constructing/loading the native facade opens no window.
// Owns its controller, borrows Session/Browser. All calls on the desktop thread.
class Viewer {
public:
    explicit Viewer(Session& session);
    ~Viewer();
    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;
    Controller& controller();
    void show();
    void hide();
    int exec();
    void refresh();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
