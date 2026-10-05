#ifndef PROWSETK_PLUGINS_BASIC_GUI_HPP
#define PROWSETK_PLUGINS_BASIC_GUI_HPP

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <prowsetk/browser.hpp>

namespace prowsetk::basic_gui {

inline constexpr std::size_t max_snapshot_bytes = 1024u * 1024u;
inline constexpr std::size_t max_snapshot_nodes = 4096;
inline constexpr std::size_t max_snapshot_depth = 64;
inline constexpr std::size_t max_activity_entries = 512;

struct InspectedNode {
    std::string path;
    std::string tag;
    std::vector<Attribute> attributes;
    std::size_t depth = 0;
    bool hidden = false;
    bool disabled = false;
};

struct Snapshot {
    std::uint64_t revision = 0;
    std::string url;
    std::string title;
    std::string preview_html;
    std::string source_html;
    std::vector<InspectedNode> nodes;
    bool limited = false;
};

// Pure, display-free projection of canonical ProwseEvent records. Source and
// attributes are sanitized; preview markup never contains resource URLs.
Snapshot inspect_document(const Document& document, std::uint64_t revision = 0);

struct MarionetteResult {
    unsigned steps = 0;
    bool stopped = false;
    std::string reason;
};

struct Activity {
    std::string type;
    std::string detail;
    std::string url;
};

// Borrows the session and its Browser, which must outlive the controller.
// All calls, including event delivery, belong on one serialized host thread.
// Activity and projections never persist page values to disk or host logs.
class Controller {
public:
    explicit Controller(Session& session);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    void navigate(std::string_view url);
    void load_html(std::string_view html, std::string_view base_url = "https://offline.test/");
    void back();
    void forward();
    void reload();
    bool can_back() const noexcept;
    bool can_forward() const noexcept;
    void refresh();
    void pump();
    void resize_viewport(int width, int height);
    // Preview/inspector actions refer to a revision and exact node identity;
    // replaced documents and detached nodes cannot redirect a stale action.
    bool click(std::size_t node, std::uint64_t revision);
    bool type(std::size_t node, std::uint64_t revision, std::string_view text);
    std::optional<std::size_t> find(std::string_view css_selector) const;
    std::string evaluate(std::string_view javascript);
    // Explicit, separate OpenCode IPC through opencode-bridge. Empty URL uses
    // OPENCODE_BASE_URL; credentials stay in the server authentication environment.
    // Link prowsetk_basic_gui. Replies are advisory and hidden unless opted in.
    void set_opencode_base_url(std::string_view url);
    const std::string& opencode_base_url() const noexcept { return opencode_base_url_; }
    void check_opencode(NetworkClient* agent_transport = nullptr);
    std::string ask_opencode(std::string_view prompt, NetworkClient* agent_transport = nullptr);
    // Trusted Lua main(args) returns version-1 decisions JSON. The host runs
    // the bounded OpenCode loop on this session. Link prowsetk_basic_gui.
    // A supplied agent transport is borrowed for this synchronous call only.
    MarionetteResult run_marionette(std::string_view lua, std::string_view goal = {},
                                   NetworkClient* agent_transport = nullptr);
    MarionetteResult run_marionette_file(const std::filesystem::path& path,
                                        std::string_view goal = {});
    void show_console_values(bool enabled) noexcept { show_values_ = enabled; }
    bool console_values_visible() const noexcept { return show_values_; }

    Session& session() noexcept { return session_; }
    const Snapshot& snapshot() const noexcept { return snapshot_; }
    const std::deque<Activity>& activity() const noexcept { return activity_; }
    std::size_t dropped_activity() const noexcept { return dropped_; }
    void clear_activity();

private:
    void record(const Event& event);
    void remember_navigation();
    std::shared_ptr<Element> action_node(std::size_t index, std::uint64_t revision) const;
    Session& session_;
    SubscriptionId subscription_ = 0;
    Snapshot snapshot_;
    std::shared_ptr<Document> snapshot_document_;
    std::vector<std::shared_ptr<Element>> action_nodes_;
    std::deque<Activity> activity_;
    std::size_t dropped_ = 0;
    std::vector<std::string> history_;
    std::size_t history_index_ = 0;
    std::optional<std::string> offline_html_;
    std::string offline_base_;
    bool dirty_ = true;
    bool show_values_ = false;
    bool marionette_running_ = false;
    std::string opencode_base_url_;
};

// Optional FLTK adapter. No FLTK headers enter the public API. Window creation
// is explicit, on the desktop/main thread. Session/Browser must outlive it.
// Construction throws Error(Unsupported) in a GUI-disabled build.
class Viewer {
public:
    explicit Viewer(Session& session);
    ~Viewer();
    Viewer(const Viewer&) = delete;
    Viewer& operator=(const Viewer&) = delete;
    void show();
    void navigate(std::string_view url);
    void load_html(std::string_view html, std::string_view base_url = "https://offline.test/");
    void refresh();
    void close();
    void set_opencode_base_url(std::string_view url);
    MarionetteResult run_marionette(std::string_view lua, std::string_view goal = {});
    MarionetteResult run_marionette_file(const std::filesystem::path& path,
                                        std::string_view goal = {});
    int exec();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reports whether the FLTK adapter was built. Defined by the model library so
// a consumer of `ProwseTk::basic_gui_model` can query it without FLTK headers
// or libraries.
bool available() noexcept;
int run(Session& session);

}  // namespace prowsetk::basic_gui
#endif
