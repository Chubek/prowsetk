#include "prowsetk/embedding.hpp"

#include "prowsetk/error.hpp"

namespace prowsetk {

EmbeddedBrowser::EmbeddedBrowser(BrowserConfig browser_config,
                                 SessionConfig session_config)
    : browser_(std::move(browser_config)),
      session_(browser_.create_session(std::move(session_config))) {}

const Document& EmbeddedBrowser::load_html(std::string_view html,
                                           std::string_view base_url) {
    session_->load_html(html, base_url);
    return document();
}

const Document& EmbeddedBrowser::navigate(std::string_view url) {
    session_->navigate(url);
    return document();
}

const Document& EmbeddedBrowser::document() const {
    const auto current = session_->document();
    if (current == nullptr || !current->valid()) {
        throw Error(ErrorCode::NotFound, "embedded browser has no loaded document");
    }
    return *current;
}

ProwseEventStream EmbeddedBrowser::events() const {
    return emit_prowse_events(document());
}

EventStreamResult EmbeddedBrowser::stream_events(
    const EventStreamVisitor& visitor) const {
    if (!visitor) {
        throw Error(ErrorCode::InvalidArgument,
                    "embedded event stream visitor must not be empty");
    }
    EventStreamResult result;
    const auto stream = events();
    for (const auto& event : stream) {
        ++result.delivered;
        if (visitor(event) == EventStreamControl::Stop) {
            result.complete = false;
            break;
        }
    }
    return result;
}

}  // namespace prowsetk
