#ifndef PROWSETK_PLUGINS_AI_ORACLE_HPP
#define PROWSETK_PLUGINS_AI_ORACLE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "prowsetk/document.hpp"
#include "prowsetk/network_client.hpp"

namespace prowsetk::plugins::ai_oracle {

enum class Task { Advice, Captcha, Crawl };
const char* to_string(Task task) noexcept;
Task parse_task(std::string_view task);

struct OracleOptions {
    bool enabled = false;
    std::string base_url = "https://api.openai.com/v1";
    std::string api_key;
    std::string model = "gpt-4o-mini";
    std::string organization;
    std::string project;
    int timeout_ms = 30000;
    std::size_t max_input_bytes = 256u * 1024u;
    std::size_t max_response_bytes = 1024u * 1024u;
    std::size_t max_requests = 16;
    std::uint32_t max_output_tokens = 512;
};

// Reads OPENAI_API_KEY, OPENAI_ORG_ID and OPENAI_PROJECT_ID only. The endpoint
// remains explicit and the oracle remains disabled until the host enables it.
OracleOptions options_from_environment();
void validate_options(const OracleOptions& options);

struct OracleRequest {
    Task task = Task::Advice;
    std::string prompt;
    // An object. Credential-like keys and URL secrets are redacted recursively.
    std::string context_json = "{}";
    // Optional page snapshot, sanitized on a detached DOM before transmission.
    std::string html;
    std::string page_url;
    // Up to four embedded PNG/JPEG data URLs, included in the input byte budget.
    // Caller-supplied images are transmitted verbatim; no remote image fetches.
    std::vector<std::string> images;
    bool json_output = false;
};

struct OracleResult {
    std::string answer;
    std::string response_id;
    std::string model;
    std::string provenance = "openai-responses";
    bool advisory = true;
    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
};

// Synchronous, stateless inquiries. Serialize access to each instance. The
// borrowed host transport must outlive this object, honor request limits, and
// never follow redirects. No site cookies or session headers are attached.
// Errors are prowsetk::Error with fixed, secret-free messages. Failed sends
// consume the attempt budget; there are no implicit retries or browser actions.
class Oracle {
public:
    explicit Oracle(NetworkClient& network, OracleOptions options = {});
    OracleResult ask(const OracleRequest& request);
    OracleResult ask_document(const Document& document, std::string prompt,
                              Task task = Task::Advice, bool json_output = false);
    std::size_t request_count() const noexcept { return request_count_; }

private:
    NetworkClient& network_;
    OracleOptions options_;
    std::size_t request_count_ = 0;
};

}  // namespace prowsetk::plugins::ai_oracle

#endif  // PROWSETK_PLUGINS_AI_ORACLE_HPP
