#ifndef PROWSETK_EVENT_HPP
#define PROWSETK_EVENT_HPP

#include <any>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prowsetk {

// Browser lifecycle and processing events. Handlers receive a mutable Event so
// they may cancel or annotate operations before the engine acts on them.
enum class EventType {
    SessionCreated,
    SessionDestroyed,
    BeforeNavigation,
    AfterNavigation,
    BeforeRequest,
    AfterResponse,
    BeforeRedirect,
    DocumentCreated,
    BeforeScript,
    AfterScript,
    ScriptException,
    Console,
    DomMutation,
    CookieChange,
    StorageAccess,
    UnsupportedApi,
    PluginInit,
    PluginShutdown,
    AntiBotDetected,
    EndpointDiscovered
};

const char* to_string(EventType type) noexcept;

// Reverse lookup for `to_string`. Returns nullopt for unknown names.
std::optional<EventType> parse_event_type(std::string_view name) noexcept;

struct Event {
    EventType type = EventType::Console;
    std::string url;
    std::string name;
    std::string message;
    std::map<std::string, std::string> attributes;
    std::any payload;
    bool cancelled = false;
    // The session that produced the event, when the event originates in one.
    // Empty for browser-global events. Used by Lua `session:on` scoping.
    std::string session_id;
};

using SubscriptionId = std::uint64_t;

class EventDispatcher {
public:
    using Handler = std::function<void(Event&)>;

    SubscriptionId subscribe(EventType type, Handler handler);
    SubscriptionId subscribe_all(Handler handler);
    bool unsubscribe(SubscriptionId id);
    void clear();

    // Dispatches to every handler registered for the event type, in
    // registration order, followed by global handlers.
    void emit(Event& event) const;

    std::size_t handler_count(EventType type) const;

private:
    struct Entry {
        SubscriptionId id;
        EventType type;
        bool global;
        std::shared_ptr<Handler> handler;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> handlers_;
    SubscriptionId next_id_ = 1;
};

}  // namespace prowsetk

#endif  // PROWSETK_EVENT_HPP
