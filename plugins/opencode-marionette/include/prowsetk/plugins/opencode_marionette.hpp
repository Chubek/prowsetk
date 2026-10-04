#ifndef PROWSETK_OPENCODE_MARIONETTE_HPP
#define PROWSETK_OPENCODE_MARIONETTE_HPP
#include <filesystem>
#include <string>
#include <vector>
#include "opencode_bridge.hpp"
#include "prowsetk/plugins/schema_grabber.hpp"

namespace prowsetk::plugins::opencode_marionette {
struct Action {
    std::string id;
    std::string kind; // click, type, navigate
    std::string selector;
    std::string value; // trusted file only; never sent to OpenCode
    std::string url;
    unsigned max_uses = 1;
};
// Strict, bounded JSON policy. Unknown keys and duplicate keys are errors.
struct Decisions {
    std::string goal;
    unsigned max_steps = 16;
    unsigned max_page_requests = 128;
    unsigned max_get_probes = 32;
    std::vector<Action> actions;
};
Decisions parse_decisions(std::string_view json);
Decisions load_decisions(const std::filesystem::path& path);
// Returns "stop" or an allowed action id. No model-generated code or values.
std::string parse_choice(std::string_view json, const Decisions& decisions);
struct Result {
    schema_grabber::SchemaGrabberResult extraction;
    unsigned steps = 0;
    bool stopped = false;
    // Discovery is always incomplete/heuristic, even after an agent stop.
    bool coverage_complete = false;
    std::string reason;
};
// Explicit synchronous call on the owning session thread. Borrowed OpenCode
// NetworkClient is separate from the page Session (no page cookies/headers).
// Session must already contain a document and an absolute HTTP(S) URL.
// Same-origin request policy lives only for this call. No automatic SPA probe.
Result run(Session& session, opencode_bridge::OpenCodeClient& client,
           const Decisions& decisions);
}
#endif
