#include <prowsetk/plugins/basic_gui.hpp>

#include <algorithm>

#include <prowsetk/error.hpp>
#include <prowsetk/redaction.hpp>
#include <prowsetk/url.hpp>

namespace prowsetk::basic_gui {
namespace {
ScriptOptions checkpoint_options() {
    ScriptOptions options;
    options.timeout_ms = 100;
    options.max_microtask_jobs = 500;
    return options;
}
std::string network_url(std::string_view url) {
    // Activity intentionally omits *all* queries, including unknown secret names.
    try {
        auto parsed = parse_url(url);
        if (parsed.scheme != "http" && parsed.scheme != "https") return "[URL omitted]";
        parsed.userinfo.clear(); parsed.query.clear(); parsed.has_query = false;
        parsed.fragment.clear(); parsed.has_fragment = false;
        return parsed.to_string().substr(0, 1024);
    } catch (...) { return "[URL omitted]"; }
}
}  // namespace

Controller::Controller(Session& session) : session_(session) {
    // An already-loaded page is inspectable immediately; an empty session
    // yields an empty snapshot until the first navigation or refresh.
    if (session_.document()) { remember_navigation(); refresh(); }
    subscription_ = session_.events().subscribe_all([this](Event& event) {
        if (event.session_id == session_.id()) record(event);
    });
}
Controller::~Controller() { session_.events().unsubscribe(subscription_); }

void Controller::record(const Event& event) {
    if (event.type == EventType::DomMutation || event.type == EventType::DocumentCreated) dirty_ = true;
    Activity entry;
    entry.type = to_string(event.type);
    entry.url = network_url(event.url);
    if (event.type == EventType::Console) {
        const auto level = event.name;
        entry.detail = (level == "log" || level == "info" || level == "warn" || level == "error" || level == "debug") ? level : "console";
        entry.detail += show_values_ ? ": " + event.message.substr(0, 1024) : ": [message hidden]";
    } else if (event.type == EventType::ScriptException) {
        entry.detail = "page script failed [error values hidden]";
    } else if (event.type == EventType::BeforeRequest || event.type == EventType::AfterResponse) {
        if (const auto method = event.attributes.find("method"); method != event.attributes.end()) {
            const auto& value = method->second;
            if (value == "GET" || value == "POST" || value == "PUT" || value == "PATCH" || value == "DELETE" || value == "HEAD" || value == "OPTIONS") entry.detail = value;
        }
        if (const auto status = event.attributes.find("status"); status != event.attributes.end()) {
            if (status->second.size() == 3 && std::all_of(status->second.begin(), status->second.end(), [](char c) { return c >= '0' && c <= '9'; })) entry.detail += " " + status->second;
        }
    }
    if (activity_.size() == max_activity_entries) { activity_.pop_front(); ++dropped_; }
    activity_.push_back(std::move(entry));
}

void Controller::clear_activity() { activity_.clear(); dropped_ = 0; }

void Controller::remember_navigation() {
    const auto& url = session_.current_url();
    if (url.empty() || (!history_.empty() && history_[history_index_] == url)) return;
    if (!history_.empty()) history_.resize(history_index_ + 1);
    history_.push_back(url);
    if (history_.size() > 64) history_.erase(history_.begin());
    history_index_ = history_.size() - 1;
}

void Controller::navigate(std::string_view url) {
    const auto parsed = parse_url(url);
    if (parsed.scheme != "https" && parsed.scheme != "http") throw Error(ErrorCode::InvalidArgument, "HTTP(S) URL required");
    const auto previous = session_.document();
    session_.navigate(url);
    if (session_.document() != previous) { offline_html_.reset(); remember_navigation(); }
    refresh();
}
void Controller::load_html(std::string_view html, std::string_view base_url) {
    session_.load_html(html, base_url);
    offline_html_ = std::string(html);
    offline_base_ = std::string(base_url);
    refresh();
}
bool Controller::can_back() const noexcept { return !history_.empty() && history_index_ > 0; }
bool Controller::can_forward() const noexcept { return !history_.empty() && history_index_ + 1 < history_.size(); }
void Controller::back() {
    if (!can_back()) return;
    auto previous = session_.document();
    session_.navigate(history_[history_index_ - 1]);
    if (session_.document() != previous) { --history_index_; offline_html_.reset(); }
    refresh();
}
void Controller::forward() {
    if (!can_forward()) return;
    auto previous = session_.document();
    session_.navigate(history_[history_index_ + 1]);
    if (session_.document() != previous) { ++history_index_; offline_html_.reset(); }
    refresh();
}
void Controller::reload() {
    if (offline_html_) session_.load_html(*offline_html_, offline_base_);
    else if (!session_.current_url().empty()) session_.navigate(session_.current_url());
    refresh();
}
void Controller::refresh() {
    const auto doc = session_.document();
    auto next = doc ? inspect_document(*doc, snapshot_.revision + 1) : Snapshot{};
    if (!doc) next.revision = snapshot_.revision + 1;
    auto nodes = doc && !next.limited ? doc->query_selector_all("*") : std::vector<std::shared_ptr<Element>>{};
    if (nodes.size() != next.nodes.size()) {
        throw Error(ErrorCode::InvalidArgument, "inspection projection identity mismatch");
    }
    snapshot_ = std::move(next);
    snapshot_document_ = doc;
    action_nodes_ = std::move(nodes);
    dirty_ = false;
}
void Controller::pump() {
    const auto previous = session_.document();
    session_.pump_events(checkpoint_options());
    if (session_.document() != previous) { offline_html_.reset(); remember_navigation(); }
    if (dirty_ || session_.document() != snapshot_document_) refresh();
}
void Controller::resize_viewport(int width, int height) {
    const auto previous = session_.document();
    session_.set_viewport({width, height, 1.0}, checkpoint_options());
    if (session_.document() != previous) { offline_html_.reset(); remember_navigation(); }
    if (dirty_ || session_.document() != snapshot_document_) refresh();
}

std::shared_ptr<Element> Controller::action_node(std::size_t index, std::uint64_t revision) const {
    if (revision != snapshot_.revision || index >= action_nodes_.size() ||
        session_.document() != snapshot_document_) return {};
    const auto& target = action_nodes_[index];
    const auto live = snapshot_document_->query_selector_all("*");
    if (std::none_of(live.begin(), live.end(), [&](const auto& item) { return item->node() == target->node(); })) return {};
    return target;
}
bool Controller::click(std::size_t node, std::uint64_t revision) {
    const auto target = action_node(node, revision);
    if (!target) return false;
    const auto previous = session_.document();
    bool result = false;
    if (!session_.browser().config().javascript && target->tag_name() == "a" && target->has_attribute("href")) {
        const auto snapshot = inspect_document(*session_.document());
        if (node < snapshot.nodes.size() && !snapshot.nodes[node].hidden && !snapshot.nodes[node].disabled) {
            navigate(resolve_url(session_.document()->base_url(), target->attribute("href")));
            result = session_.document() != previous;
        }
    } else result = session_.click_element(target);
    if (session_.document() != previous) { offline_html_.reset(); remember_navigation(); }
    refresh();
    return result;
}
bool Controller::type(std::size_t node, std::uint64_t revision, std::string_view text) {
    const auto target = action_node(node, revision);
    if (!target || text.size() > 4096 || target->has_attribute("readonly") ||
        (target->tag_name() != "input" && target->tag_name() != "textarea")) return false;
    const auto previous = session_.document();
    const bool result = session_.type_element(target, text);
    if (session_.document() != previous) { offline_html_.reset(); remember_navigation(); }
    refresh();
    return result;
}
std::optional<std::size_t> Controller::find(std::string_view css_selector) const {
    const auto doc = session_.document();
    if (!doc || doc != snapshot_document_) return {};
    const auto target = doc->query_selector(css_selector);
    if (!target) return {};
    for (std::size_t i = 0; i < action_nodes_.size(); ++i) if (action_nodes_[i]->node() == target->node()) return i;
    return {};
}
std::string Controller::evaluate(std::string_view javascript) {
    if (javascript.size() > 64u * 1024u) throw Error(ErrorCode::ResourceLimit, "inspection script limit");
    auto result = session_.evaluate_js(javascript, checkpoint_options());
    pump();
    refresh();
    return show_values_ ? result.substr(0, 4096) : "[result hidden; enable Console values to view]";
}

}  // namespace prowsetk::basic_gui
