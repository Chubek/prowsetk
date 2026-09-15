#include "prowsetk/event.hpp"

#include <algorithm>

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

SubscriptionId EventDispatcher::subscribe(EventType type, Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    const SubscriptionId id = next_id_++;
    handlers_.push_back(Entry{id, type, false, std::move(handler)});
    return id;
}

SubscriptionId EventDispatcher::subscribe_all(Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    const SubscriptionId id = next_id_++;
    handlers_.push_back(Entry{id, EventType::Console, true, std::move(handler)});
    return id;
}

bool EventDispatcher::unsubscribe(SubscriptionId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::remove_if(
        handlers_.begin(), handlers_.end(),
        [id](const Entry& entry) { return entry.id == id; });
    if (it == handlers_.end()) {
        return false;
    }
    handlers_.erase(it, handlers_.end());
    return true;
}

void EventDispatcher::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.clear();
}

void EventDispatcher::emit(Event& event) const {
    std::vector<Entry> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = handlers_;
    }
    for (const auto& entry : snapshot) {
        if (entry.global || entry.type == event.type) {
            entry.handler(event);
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
