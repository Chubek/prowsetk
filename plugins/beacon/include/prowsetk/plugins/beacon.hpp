#ifndef PROWSETK_PLUGINS_BEACON_HPP
#define PROWSETK_PLUGINS_BEACON_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/redaction.hpp"

namespace prowsetk::plugins::beacon {

// Beacon is a ProwseTk oracle plugin that bridges ProwseTk drivers to Firefox
// through `beacond` and a Native Messaging host (plugins/beacon/AGENTS.md).
// Firefox is the server; Beacon is the client. A "Flash" is a driver request
// for browser-side assistance (live DOM, network info, stylesheet, or a
// tracepoint subscription). The addon fulfills Flashes only after explicit
// user consent ("Connect to Flash"); nothing is exfiltrated automatically.
//
// This header is dependency-free apart from the STL and the engine Redactor.
// All network access stays host-mediated: the core library never opens
// sockets. The `beacond` daemon and the Native Messaging host (POSIX Unix
// socket IPC) link this header but live in separate translation units so a
// WASM-disabled, network-free build still configures and tests cleanly.
// Discovery is heuristic and never authoritative; secrets stay redacted.

inline constexpr std::size_t kMaxNativeMessageBytes = 8u * 1024u * 1024u;
inline constexpr std::uint32_t kDefaultBeaconTimeoutMs = 30000;
inline constexpr std::uint32_t kDefaultMaxFlashes = 64;
inline constexpr const char* kDefaultSocketPath = "/tmp/beacond.sock";
inline constexpr const char* kBeaconAddonId = "prowsetk-beacon@example.com";
inline constexpr const char* kBeaconNativeHostName = "prowsetk_beacon";

enum class RequestType {
    PageDom,
    NetworkInfo,
    Stylesheet,
    Tracepoint,
};

enum class FlashStatus {
    Seeking,
    Connected,
    Closed,
    TimedOut,
};

enum class TracepointEventKind {
    DomMutation,
    NetworkRequest,
    StyleChange,
};

const char* to_string(RequestType type) noexcept;
const char* to_string(FlashStatus status) noexcept;
const char* to_string(TracepointEventKind kind) noexcept;

std::optional<RequestType> parse_request_type(std::string_view name) noexcept;
std::optional<FlashStatus> parse_flash_status(std::string_view name) noexcept;
std::optional<TracepointEventKind> parse_tracepoint_event(
    std::string_view name) noexcept;

bool is_valid_resource_type(std::string_view name) noexcept;

struct FlashFilters {
    std::string url_pattern;
    std::vector<std::string> resource_types;
};

struct FlashRequest {
    std::string flash_id;
    RequestType request_type = RequestType::PageDom;
    FlashFilters filters;
};

struct FlashDescriptor {
    std::string flash_id;
    RequestType request_type = RequestType::PageDom;
    FlashFilters filters;
    FlashStatus status = FlashStatus::Seeking;
};

struct FlashConnect {
    std::string flash_id;
    int tab_id = -1;
};

struct FlashData {
    std::string flash_id;
    std::string data_type = "page_dom";
    std::string html;
    std::string url;
    long long timestamp = 0;
};

struct TracepointEvent {
    std::string flash_id;
    TracepointEventKind event = TracepointEventKind::DomMutation;
    std::string selector;
    std::string detail;
};

struct BeaconOptions {
    std::string socket_path = kDefaultSocketPath;
    // Secret-bearing. Never logged. Empty means socket-permission auth only.
    std::string auth_token;
    std::uint32_t timeout_ms = kDefaultBeaconTimeoutMs;
    std::uint32_t max_flashes = kDefaultMaxFlashes;
    bool require_user_consent = true;
    bool redact_secrets = true;
};

// Generates a uuid-v4-shaped Flash identifier.
std::string make_flash_id();

// Builds a Flash request, generating an id when `flash_id` is empty.
FlashRequest new_flash(RequestType type, FlashFilters filters,
                       std::string flash_id = {});

bool validate_flash_request(const FlashRequest& request,
                            std::string* error_out = nullptr);
bool validate_flash_filters(const FlashFilters& filters,
                            std::string* error_out = nullptr);

std::string json_escape(std::string_view value);

// Deterministic JSON renderers for the Flash protocol (AGENTS.md "Message
// Protocol"). URL patterns and payload URLs are redacted through `redactor`
// when `redact` is true; HTML payloads are carried verbatim because they are
// user-consented page data, never secrets from the driver.
std::string render_flash_request(const FlashRequest& request,
                                 const Redactor& redactor = Redactor(),
                                 bool redact = true);
std::string render_flash_list(const std::vector<FlashDescriptor>& flashes,
                              const Redactor& redactor = Redactor(),
                              bool redact = true);
std::string render_flash_connect(const FlashConnect& connect);
std::string render_flash_data(const FlashData& data,
                              const Redactor& redactor = Redactor(),
                              bool redact = true);
std::string render_tracepoint_event(const TracepointEvent& event);
std::string render_flash_disconnect(const std::string& flash_id);
std::string render_ping();

// Minimal best-effort parsers for round-trip tests and the native host.
// They extract the fields this plugin emits; unknown fields are ignored and
// malformed input yields nullopt rather than throwing.
std::optional<FlashRequest> parse_flash_request(std::string_view json);
std::optional<FlashConnect> parse_flash_connect(std::string_view json);
std::optional<std::string> parse_message_type(std::string_view json);
std::optional<std::string> parse_flash_id(std::string_view json);
std::optional<int> parse_tab_id(std::string_view json);

// Native Messaging framing: 4-byte little-endian length prefix + UTF-8 JSON
// (Firefox stdio protocol). Empty vector / false means oversize or corrupt.
std::vector<std::uint8_t> encode_native_message(std::string_view json);
bool decode_native_message(const std::vector<std::uint8_t>& bytes,
                           std::string* json_out,
                           std::string* error_out = nullptr);
bool decode_native_message(const std::string& bytes, std::string* json_out,
                           std::string* error_out = nullptr);

// In-memory Flash session store. Shared by the `beacond` daemon and by unit
// tests as a hermetic fake: no sockets, no wall-clock sleeps. Callers inject
// `now_ms` (monotonic milliseconds) so expiry is deterministic.
class FlashSessionManager {
public:
    struct Entry {
        FlashDescriptor descriptor;
        int tab_id = -1;
        std::uint64_t created_ms = 0;
        std::uint64_t last_activity_ms = 0;
        std::deque<std::string> messages;
    };

    explicit FlashSessionManager(BeaconOptions options = {},
                                 Redactor redactor = Redactor());

    const BeaconOptions& options() const noexcept { return options_; }

    bool create(const FlashRequest& request, std::uint64_t now_ms,
                std::string* error_out = nullptr);
    bool connect(const std::string& flash_id, int tab_id,
                 std::uint64_t now_ms, std::string* error_out = nullptr);
    // Either side may terminate; unknown ids report an error and return false.
    bool disconnect(const std::string& flash_id,
                    std::string* error_out = nullptr);
    // Drops entries idle longer than timeout_ms. Returns the removal count.
    std::size_t expire_stale(std::uint64_t now_ms);

    std::vector<FlashDescriptor> list() const;
    std::vector<FlashDescriptor> list_seeking() const;
    std::optional<FlashDescriptor> find(std::string_view flash_id) const;
    std::optional<int> connected_tab(std::string_view flash_id) const;
    // Accept only data from the tab associated with a connected Flash.
    // The broker bounds both individual messages and queued delivery.
    bool publish(std::string_view flash_id, int tab_id, std::string message,
                 std::uint64_t now_ms, std::string* error_out = nullptr);
    std::optional<std::string> poll(std::string_view flash_id,
                                    std::uint64_t now_ms,
                                    std::string* error_out = nullptr);
    std::size_t size() const noexcept { return entries_.size(); }
    void clear();

    // Renders the current seeking/connected set as a flash_list message.
    std::string render_list() const;

private:
    BeaconOptions options_;
    Redactor redactor_;
    std::map<std::string, Entry, std::less<>> entries_;
};

std::vector<std::string> beacon_capabilities();

}  // namespace prowsetk::plugins::beacon

#endif  // PROWSETK_PLUGINS_BEACON_HPP
