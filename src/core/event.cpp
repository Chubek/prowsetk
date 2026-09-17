#include "prowsetk/event.hpp"

#include <algorithm>
#include <optional>

namespace prowsetk {

const char* to_string(EventType type) noexcept {
    switch (type) {
        case EventType::SessionCreated: return "session_created";
        case EventType::SessionDestroyed: return "session_destroyed";
        case EventType::BeforeNavigation: return "before_navigation";
        case EventType::AfterNavigation: return "after_navigation";
        case EventType::BeforeRequest: return "before_request";
        case EventType::AfterResponse: return "after_response";
        case EventType::BeforeRedirect: return "before_redirect";
        case EventType::DocumentCreated: return "document_created";
        case EventType::BeforeScript: return "before_script";
        case EventType::AfterScript: return "after_script";
        case EventType::ScriptException: return "script_exception";
        case EventType::Console: return "console";
        case EventType::DomMutation: return "dom_mutation";
        case EventType::CookieChange: return "cookie_change";
        case EventType::StorageAccess: return "storage_access";
        case EventType::UnsupportedApi: return "unsupported_api";
        case EventType::PluginInit: return "plugin_init";
        case EventType::PluginShutdown: return "plugin_shutdown";
        case EventType::EndpointDiscovered: return "endpoint_discovered";
    }
    return "unknown";
}

std::optional<EventType> parse_event_type(std::string_view name) noexcept {
    static const std::vector<std::pair<std::string_view, EventType>> names = {
        {"session_created", EventType::SessionCreated},
        {"session_destroyed", EventType::SessionDestroyed},
        {"before_navigation", EventType::BeforeNavigation},
        {"after_navigation", EventType::AfterNavigation},
        {"before_request", EventType::BeforeRequest},
        {"after_response", EventType::AfterResponse},
        {"before_redirect", EventType::BeforeRedirect},
        {"document_created", EventType::DocumentCreated},
        {"before_script", EventType::BeforeScript},
        {"after_script", EventType::AfterScript},
        {"script_exception", EventType::ScriptException},
        {"console", EventType::Console},
        {"dom_mutation", EventType::DomMutation},
        {"cookie_change", EventType::CookieChange},
        {"storage_access", EventType::StorageAccess},
        {"unsupported_api", EventType::UnsupportedApi},
        {"plugin_init", EventType::PluginInit},
        {"plugin_shutdown", EventType::PluginShutdown},
        {"endpoint_discovered", EventType::EndpointDiscovered},
    };
    for (const auto& [candidate, type] : names) {
        if (candidate == name) {
            return type;
        }
    }
    return std::nullopt;
}

SubscriptionId EventDispatcher::subscribe(EventType type, Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    const SubscriptionId id = next_id_++;
    handlers_.push_back(
        Entry{id, type, false, std::make_shared<Handler>(std::move(handler))});
    return id;
}

SubscriptionId EventDispatcher::subscribe_all(Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    const SubscriptionId id = next_id_++;
    handlers_.push_back(Entry{
        id, EventType::Console, true,
        std::make_shared<Handler>(std::move(handler))});
    return id;
}

bool EventDispatcher::unsubscribe(SubscriptionId id) {
    std::shared_ptr<Handler> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(
            handlers_.begin(), handlers_.end(),
            [id](const Entry& entry) { return entry.id == id; });
        if (it == handlers_.end()) {
            return false;
        }
        removed = std::move(it->handler);
        handlers_.erase(it);
    }
    return true;
}

void EventDispatcher::clear() {
    std::vector<Entry> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        removed.swap(handlers_);
    }
}

void EventDispatcher::emit(Event& event) const {
    std::vector<Entry> snapshot;
    const EventType type = event.type;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : handlers_) {
            if (entry.global || entry.type == type) {
                snapshot.push_back(entry);
            }
        }
    }
    for (const auto& entry : snapshot) {
        if (!entry.global) {
            (*entry.handler)(event);
        }
    }
    for (const auto& entry : snapshot) {
        if (entry.global) {
            (*entry.handler)(event);
        }
    }
}

std::size_t EventDispatcher::handler_count(EventType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        handlers_.begin(), handlers_.end(),
        [type](const Entry& entry) { return entry.type == type; }));
}

}  // namespace prowsetk
