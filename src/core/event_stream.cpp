#include "core/event_stream.hpp"
#include "prowsetk/error.hpp"

namespace prowsetk {
void EventStreamBudget::consume(const ProwseEvent& event) {
    if (++events_ > max_event_stream_events ||
        event.attributes.size() > max_event_stream_events - attributes_)
        throw Error(ErrorCode::ResourceLimit, "event stream exceeds record bound");
    attributes_ += event.attributes.size();
    const auto charge = [&](std::size_t size) {
        if (size > max_event_stream_bytes - bytes_)
            throw Error(ErrorCode::ResourceLimit, "event stream exceeds byte bound");
        bytes_ += size;
    };
    charge(event.kind.size()); charge(event.xpath.size());
    charge(event.tag.size()); charge(event.name.size());
    charge(event.value.size()); charge(event.subtree_text.size());
    for (const auto& attribute : event.attributes) {
        charge(attribute.name.size()); charge(attribute.value.size());
    }
}

EventStreamReader::EventStreamReader(const Document& document)
    : EventStreamReader(emit_prowse_events(document)) {}

EventStreamReader::EventStreamReader(ProwseEventStream events)
    : events_(std::move(events)) {
    if (events_.size() > max_event_stream_events)
        throw Error(ErrorCode::ResourceLimit, "event stream exceeds event bound");
    EventStreamBudget budget;
    for (const auto& event : events_) budget.consume(event);
}

bool EventStreamReader::replay(const EventStreamCallback& callback, EventStreamOptions options) const {
    if (!callback) throw Error(ErrorCode::InvalidArgument, "event stream callback must not be empty");
    if (options.max_events > max_event_stream_events) throw Error(ErrorCode::InvalidArgument, "event stream limit exceeds bound");
    if (events_.size() > options.max_events) return false;
    for (const auto& event : events_) {
        if (callback(event) == EventStreamAction::Stop) break;
    }
    return true;
}
}
