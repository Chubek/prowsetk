#ifndef PROWSETK_CORE_EVENT_STREAM_HPP
#define PROWSETK_CORE_EVENT_STREAM_HPP

#include <functional>
#include <span>
#include "prowsetk/event.hpp"

namespace prowsetk {

inline constexpr std::size_t max_event_stream_events = 400000;
inline constexpr std::size_t max_event_stream_bytes = 32u * 1024u * 1024u;
enum class EventStreamAction { Continue, Stop };
using EventStreamCallback = std::function<EventStreamAction(const ProwseEvent&)>;
struct EventStreamOptions {
    std::size_t max_events = max_event_stream_events;
};

// Shared allocation accounting, also used while the canonical model is built.
class EventStreamBudget {
public:
    void consume(const ProwseEvent& event);
private:
    std::size_t events_ = 0;
    std::size_t attributes_ = 0;
    std::size_t bytes_ = 0;
};

// Owns an immutable, bounded snapshot; never retains DOM handles. Callback
// exceptions propagate. Stop is successful and does not invalidate the source.
class EventStreamReader {
public:
    explicit EventStreamReader(const Document& document);
    explicit EventStreamReader(ProwseEventStream events);
    bool replay(const EventStreamCallback& callback,
                EventStreamOptions options = {}) const;
    std::span<const ProwseEvent> events() const noexcept { return events_; }
private:
    ProwseEventStream events_;
};

}  // namespace prowsetk
#endif
