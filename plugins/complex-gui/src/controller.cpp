#include <prowsetk/plugins/complex_gui.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/url.hpp>
#include <algorithm>

namespace prowsetk::complex_gui {
namespace {
constexpr std::size_t html_budget = 16u * 1024u * 1024u;
struct Operation {
    bool& active;
    explicit Operation(bool& flag) : active(flag) {
        if (active) throw Error(ErrorCode::InvalidArgument, "complex-gui reentry");
        active = true;
    }
    ~Operation() { active = false; }
};
void validate_url(std::string_view url) {
    if (url.size() > 8192) throw Error(ErrorCode::ResourceLimit, "URL limit");
    const auto parsed = parse_url(url);
    if ((parsed.scheme != "http" && parsed.scheme != "https") || !parsed.has_host() || !parsed.userinfo.empty())
        throw Error(ErrorCode::InvalidUrl, "HTTP(S) URL required");
}
ScriptOptions checkpoint() {
    ScriptOptions options; options.timeout_ms = 100; options.max_microtask_jobs = 500; return options;
}
}
class Controller::Loader final : public ImageLoader {
public:
    explicit Loader(Session& session) : session_(session) {}
    unsigned requests = 0;
    std::vector<std::uint8_t> load(std::string_view url) override {
        try { validate_url(url); } catch (const Error&) { return {}; }
        if (requests >= 32) return {};
        ++requests;
        HttpRequest request; request.method = "GET"; request.url = std::string(url);
        try {
            auto response = session_.request(std::move(request));
            if (response.status != 200 || response.body.size() > max_render_image_bytes) return {};
            return {response.body.begin(), response.body.end()};
        } catch (const Error&) { return {}; }
    }
private:
    Session& session_;
};
Controller::Controller(Session& session) : session_(session), loader_(std::make_unique<Loader>(session)) {
    options_.max_images = 32;
    if (session_.document()) remember();
    subscription_ = session_.events().subscribe_all([this](Event& e) {
        if (e.session_id == session_.id() && (e.type == EventType::DomMutation || e.type == EventType::DocumentCreated)) {
            dirty_ = true; ++mutation_;
        }
    });
}
Controller::~Controller() { session_.events().unsubscribe(subscription_); }
void Controller::remember(std::optional<std::string> html) {
    const auto& url = session_.current_url();
    if (!html && !history_.empty() && history_[history_index_].url == url) return;
    if (!history_.empty()) history_.resize(history_index_ + 1);
    history_.push_back({url, std::move(html)});
    std::size_t bytes = 0;
    for (const auto& entry : history_) if (entry.html) bytes += entry.html->size();
    while (history_.size() > 32 || (bytes > html_budget && history_.size() > 1)) {
        const auto& oldest = history_.front().html;
        if (oldest) bytes -= oldest->size();
        history_.erase(history_.begin());
    }
    history_index_ = history_.size() - 1;
}
void Controller::refresh_impl() {
    dirty_ = true;
    const auto doc = session_.document();
    if (doc != document_) { images_.clear(); loader_->requests = 0; }
    const auto epoch = mutation_;
    auto next = doc ? render_document(*doc, options_) : RenderedPage{};
    if (images_enabled_ && loader_->requests >= 32) next.limited = true;
    if (epoch != mutation_ || doc != session_.document())
        throw Error(ErrorCode::InvalidArgument, "document changed during render");
    page_ = std::move(next); document_ = doc; ++revision_; dirty_ = false;
}
void Controller::refresh() { Operation guard(active_); refresh_impl(); }
void Controller::navigate(std::string_view url) {
    Operation guard(active_); validate_url(url);
    const auto old = session_.document();
    session_.navigate(url);
    if (session_.document() != old) remember();
    refresh_impl();
}
void Controller::load_html(std::string_view html, std::string_view base) {
    Operation guard(active_); validate_url(base);
    if (html.size() > html_budget) throw Error(ErrorCode::ResourceLimit, "HTML limit");
    session_.load_html(html, base); remember(std::string(html)); refresh_impl();
}
bool Controller::can_back() const noexcept { return !history_.empty() && history_index_ > 0; }
bool Controller::can_forward() const noexcept { return !history_.empty() && history_index_ + 1 < history_.size(); }
void Controller::travel(std::size_t index) {
    const auto entry = history_[index];
    const auto old = session_.document();
    if (entry.html) session_.load_html(*entry.html, entry.url); else session_.navigate(entry.url);
    if (session_.document() != old) { history_index_ = index; history_[index].url = session_.current_url(); }
    refresh_impl();
}
void Controller::back() { Operation guard(active_); if (can_back()) travel(history_index_ - 1); }
void Controller::forward() { Operation guard(active_); if (can_forward()) travel(history_index_ + 1); }
void Controller::reload() {
    Operation guard(active_);
    if (!history_.empty()) travel(history_index_); else refresh_impl();
}
void Controller::pump() {
    Operation guard(active_); const auto old = session_.document();
    session_.pump_events(checkpoint());
    if (old != session_.document()) remember();
    if (dirty_ || session_.document() != document_) refresh_impl();
}
void Controller::resize(int width, int height) {
    Operation guard(active_);
    session_.set_viewport({width, height, 1.0}, checkpoint());
    options_.viewport_width = width; options_.viewport_height = height;
    refresh_impl();
}
void Controller::set_measurer(const TextMeasurer* measurer) {
    Operation guard(active_); options_.measurer = measurer; dirty_ = true;
}
void Controller::enable_images(bool enabled) {
    Operation guard(active_); images_enabled_ = enabled;
    options_.images = enabled ? &images_ : nullptr;
    options_.loader = enabled ? loader_.get() : nullptr;
    dirty_ = true;
}
std::optional<Target> Controller::target_at(double x, double y) const {
    if (dirty_ || active_ || document_ != session_.document()) return {};
    const auto hit = hit_test(page_.paint, x, y);
    if (!hit.interactive) return {};
    for (std::size_t i = page_.paint.items.size(); i > 0; --i) {
        const auto& p = page_.paint.items[i - 1];
        if (p.element && p.element->node() == hit.element->node())
            return Target{revision_, i - 1, p.control};
    }
    return {};
}
std::shared_ptr<Element> Controller::resolve(Target target) const {
    if (dirty_ || target.revision != revision_ || target.item >= page_.paint.size() ||
        !document_ || document_ != session_.document()) return {};
    auto node = page_.paint.items[target.item].element;
    for (std::size_t depth = 0; node && !is_interactive_element(*node) && depth < max_render_depth; ++depth)
        node = node->parent();
    if (!node || node->has_attribute("disabled") || node->has_attribute("readonly") ||
        node->attribute("aria-disabled") == "true") return {};
    const auto live = document_->query_selector_all("*");
    if (std::none_of(live.begin(), live.end(), [&](const auto& e) { return e->node() == node->node(); })) return {};
    return node;
}
bool Controller::click(Target target) {
    Operation guard(active_); const auto node = resolve(target); if (!node) return false;
    const auto old = session_.document();
    bool result;
    if (!session_.browser().config().javascript && node->tag_name() == "a") {
        const auto url = resolve_url(document_->base_url(), node->attribute("href"));
        validate_url(url); session_.navigate(url); result = session_.document() != old;
    } else result = session_.click_element(node);
    if (session_.document() != old) remember();
    refresh_impl(); return result;
}
bool Controller::edit(Target target, std::string_view replacement) {
    Operation guard(active_); const auto node = resolve(target); if (!node) return false;
    const auto kind = page_.paint.items[target.item].control;
    if (kind != ControlKind::TextField && kind != ControlKind::PasswordField && kind != ControlKind::TextArea) return false;
    if (replacement.size() > 4096) throw Error(ErrorCode::ResourceLimit, "input limit");
    const auto old = session_.document();
    node->set_value("");
    bool result = true;
    if (session_.browser().config().javascript) result = session_.type_element(node, replacement);
    else node->set_value(replacement);
    if (session_.document() != old) remember();
    refresh_impl(); return result;
}
std::string Controller::address() const {
    try {
        auto url = parse_url(session_.current_url());
        url.userinfo.clear(); url.query.clear(); url.has_query = false; url.fragment.clear(); url.has_fragment = false;
        return url.to_string().substr(0, 8192);
    } catch (...) { return {}; }
}
}
