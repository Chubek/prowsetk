#ifndef PROWSETK_OPENCODE_BRIDGE_HPP
#define PROWSETK_OPENCODE_BRIDGE_HPP

// Native ProwseTk <-> OpenCode bridge (plugins/opencode-bridge).
//
// Bi-directional bridge between Flatworm sessions and an OpenCode local
// HTTP/SSE server (default http://127.0.0.1:4096). All HTTP goes through the
// borrowed prowsetk::NetworkClient; the bridge never opens sockets itself.
// DOM snapshots are sanitized on detached copies before transmission, and
// errors are secret-free.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "prowsetk/document.hpp"
#include "prowsetk/network_client.hpp"

namespace prowsetk::plugins::opencode_bridge {

inline constexpr std::string_view kDefaultBaseUrl = "http://127.0.0.1:4096";
inline constexpr std::size_t kDefaultMaxInputBytes = 256u * 1024u;
inline constexpr std::size_t kDefaultMaxResponseBytes = 1024u * 1024u;
inline constexpr std::size_t kMaxInputBytesHardCap = 2u * 1024u * 1024u;
inline constexpr std::size_t kMaxResponseBytesHardCap = 4u * 1024u * 1024u;
// Documents larger than this are summarized (title + text extract) instead of
// shipping raw outer HTML into a prompt.
inline constexpr std::size_t kSanitizedHtmlBudget = 100u * 1024u;

inline constexpr std::size_t kDefaultMaxEndpointsPerCleanup = 200;

struct BridgeConfig {
    std::string base_url = std::string(kDefaultBaseUrl);
    std::string username;
    std::string password;
    int timeout_ms = 30000;
    std::size_t max_input_bytes = kDefaultMaxInputBytes;
    std::size_t max_response_bytes = kDefaultMaxResponseBytes;
    std::size_t max_requests = 16;
    // Plain HTTP is loopback-only unless this is set. HTTPS is unrestricted.
    bool allow_remote_http = false;
    // Route prefix: "" speaks the bare bridge paths (/session, ...) while
    // "/api" speaks a real OpenCode v2 server (/api/session, ...). Anything
    // else is rejected.
    std::string api_prefix;
    // Upper bound on waiting for an agent reply in prompt() under "/api".
    int prompt_wait_ms = 120000;
};

// Reads OPENCODE_SERVER_USERNAME / OPENCODE_SERVER_PASSWORD and
// OPENCODE_BASE_URL (explicit override of the loopback default).
BridgeConfig config_from_environment();
void validate_config(const BridgeConfig& config);

struct PageSnapshot {
    std::string url;
    std::string title;
    std::string html;
    std::string text;

    static PageSnapshot from_document(const Document& document);
};

// Reduce `html` for prompt inclusion on a detached parse: drops heavy
// multimedia/script/style content, truncates inline base64 assets and
// excessive SVG subtrees, strips non-semantic attributes, and falls back to a
// title/text summary when the result still exceeds `budget` bytes.
std::string sanitize_html(std::string_view html, std::size_t budget = kSanitizedHtmlBudget);
// Title/URL/text summary used for large documents.
std::string summarize_snapshot(const PageSnapshot& snapshot, std::size_t budget = kSanitizedHtmlBudget);
// Structured extraction prompt carrying goals, JSON schema, and page state.
std::string build_scrape_prompt(const PageSnapshot& snapshot, std::string_view instructions,
                                std::string_view schema);
// Endpoint-cleanup prompt over a JSON array of endpoint objects. The contract
// is subtractive only: the agent must return a JSON array containing a subset
// of the supplied endpoints (same url+method pairs), never invented ones.
// `endpoints_json` must already be redacted. Throws on empty input or when the
// prompt would exceed `max_input_bytes`.
std::string build_endpoint_cleanup_prompt(std::string_view endpoints_json,
                                          std::string_view instructions = {},
                                          std::size_t max_input_bytes = kDefaultMaxInputBytes);
// Percent-encodes a query-string value (RFC 3986 unreserved set left intact).
std::string percent_encode(std::string_view value);

// Minimal JSON helpers (dependency-free). Escape produces a quoted JSON
// string; extract_string finds a top-level string field and decodes escapes.
// Keys inside string literals never match: scanning skips them with full
// escape handling, so echoed key-like text in values cannot shadow real keys.
std::string json_escape(std::string_view text);
bool json_extract_string(std::string_view body, std::string_view key, std::string& out);

// Small bounded JSON document parser (depth-capped, no external
// dependencies) for OpenCode v2 envelopes such as {"data": {...}}.
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    // Decoded text for String, raw literal for Number.
    std::string str;
    std::vector<JsonValue> items;
    // Object members in document order. The value is boxed because the type
    // is recursive; use `*member.second` to reach it.
    std::vector<std::pair<std::string, std::unique_ptr<JsonValue>>> fields;
};
// Parses a whole document; trailing garbage fails. Returns false on any
// malformed input, excessive depth (>32), or size beyond the hard response
// cap.
bool json_parse(std::string_view body, JsonValue& out);
// Document-order lookup of nested string fields, e.g. {"data","id"}.
bool json_find_string_at_path(const JsonValue& root,
                              std::initializer_list<std::string_view> path, std::string& out);

// One assistant message from a v2 message list.
struct AssistantMessage {
    std::string id;
    // Concatenated "text" content parts (reasoning parts excluded).
    std::string text;
    bool completed = false;
    // Idle outcome markers, e.g. {"type":"idle","outcome":"succeeded"}.
    bool idle = false;
    bool succeeded = false;
};
// Extracts message records from a {"data":[...]} message list (or a bare
// array). Malformed entries are skipped, never thrown.
std::vector<AssistantMessage> parse_assistant_messages(const JsonValue& root);
// Parses a text/event-stream payload into `data:` payloads.
std::vector<std::string> parse_sse_events(std::string_view body);

// Synchronous OpenCode IPC client. The borrowed `network` must outlive this
// object. Instances are not thread-safe; serialize access. Never mutates a
// Session or Document; snapshots are caller-supplied values.
class OpenCodeClient {
public:
    OpenCodeClient(NetworkClient& network, BridgeConfig config = config_from_environment());

    const BridgeConfig& config() const noexcept { return config_; }
    std::size_t request_count() const noexcept { return request_count_; }

    // POST /session -> agent session id.
    std::string create_session();
    // POST /session/{id}/message with {text, tools?} -> answer text.
    std::string prompt(const std::string& session_id, std::string_view text,
                       const std::vector<std::string>& tools = {});
    // POST /session/{id}/prompt_async -> operation id.
    std::string prompt_async(const std::string& session_id, std::string_view text);
    // GET /event -> SSE data payloads for `session_id`.
    std::vector<std::string> stream_events(const std::string& session_id);
    // POST /session/{id}/abort.
    void abort(const std::string& session_id);

    // Sanitizes `snapshot`, builds the extraction prompt with `instructions`
    // and `schema`, sends it as a message on `session_id`, and validates that
    // the answer is a JSON object when `schema` is non-empty.
    std::string scrape_with_prompt(const std::string& session_id, const PageSnapshot& snapshot,
                                   std::string_view instructions, std::string_view schema = {});

private:
    NetworkClient& network_;
    BridgeConfig config_;
    std::size_t request_count_ = 0;

    bool use_v2_api() const noexcept;
    std::string route(std::string_view path) const;
    std::string prompt_v2(const std::string& session_id, std::string_view text);

    HttpResponse send(std::string method, std::string url, std::string body);
};

}  // namespace prowsetk::plugins::opencode_bridge

#endif  // PROWSETK_OPENCODE_BRIDGE_HPP
