#ifndef PROWSETK_EMBEDDING_HPP
#define PROWSETK_EMBEDDING_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

#include "prowsetk/browser.hpp"
#include "prowsetk/ir.hpp"

namespace prowsetk {

// Control returned by an embedder's event callback. Stopping is successful and
// leaves the session/document intact; callbacks may also throw Error to abort
// with the caller's chosen error code.
enum class EventStreamControl { Continue, Stop };

using EventStreamVisitor =
    std::function<EventStreamControl(const ProwseEvent& event)>;

struct EventStreamResult {
    std::size_t delivered = 0;
    bool complete = true;
};

// A small ownership-oriented embedding API. It owns one Browser and one
// isolated Session, exposes host-mediated navigation/loading, and presents the
// canonical IR through a bounded visitor. Browser(), Session(), and Document()
// retain the normal C++ API for configuration and advanced use.
class EmbeddedBrowser {
public:
    explicit EmbeddedBrowser(BrowserConfig browser_config = {},
                             SessionConfig session_config = {});
    ~EmbeddedBrowser() = default;

    EmbeddedBrowser(const EmbeddedBrowser&) = delete;
    EmbeddedBrowser& operator=(const EmbeddedBrowser&) = delete;

    Browser& browser() noexcept { return browser_; }
    const Browser& browser() const noexcept { return browser_; }
    Session& session() noexcept { return *session_; }
    const Session& session() const noexcept { return *session_; }

    // Load content or navigate with all network activity mediated by Session.
    // Returns the newly installed document, or throws the normal public Error.
    const Document& load_html(std::string_view html,
                              std::string_view base_url = {});
    const Document& navigate(std::string_view url);

    // Throws Error(NotFound) when no document has been installed.
    const Document& document() const;
    ProwseEventStream events() const;
    EventStreamResult stream_events(const EventStreamVisitor& visitor) const;

private:
    Browser browser_;
    std::shared_ptr<Session> session_;
};

}  // namespace prowsetk

#endif  // PROWSETK_EMBEDDING_HPP
