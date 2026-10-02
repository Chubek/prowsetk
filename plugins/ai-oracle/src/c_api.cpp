#include "prowsetk/plugins/ai_oracle.h"
#include "prowsetk/plugins/ai_oracle.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "prowsetk/error.hpp"

namespace {
namespace ai = prowsetk::plugins::ai_oracle;

[[noreturn]] void invalid() {
    throw prowsetk::Error(prowsetk::ErrorCode::InvalidArgument, "ai-oracle: invalid C arguments");
}

std::string bounded(const char* value, std::size_t limit) {
    if (!value) return {};
    std::size_t length = 0;
    while (length <= limit && value[length]) ++length;
    if (length > limit) invalid();
    return {value, length};
}

class CallbackNetwork final : public prowsetk::NetworkClient {
public:
    explicit CallbackNetwork(ProwseTkAiOracleTransport transport) : transport_(transport) {}

    prowsetk::HttpResponse send(const prowsetk::HttpRequest& request) override {
        std::vector<ProwseTkHeader> headers;
        for (const auto& [name, value] : request.headers) headers.push_back({name.c_str(), value.c_str()});
        const ProwseTkHttpRequest wire{request.method.c_str(), request.url.c_str(), headers.data(), headers.size(),
            {request.body.data(), request.body.size()}, static_cast<uint32_t>(request.timeout_ms), request.max_response_bytes};
        ProwseTkHttpResponse response{};
        if (transport_.send(transport_.user_data, &wire, &response) != PROWSETK_STATUS_OK) {
            throw prowsetk::Error(prowsetk::ErrorCode::NetworkError, "ai-oracle: host transport failed");
        }
        if (response.body.size > request.max_response_bytes) {
            throw prowsetk::Error(prowsetk::ErrorCode::ResourceLimit, "ai-oracle: response byte limit exceeded");
        }
        if (!response.body.data && response.body.size) invalid();
        prowsetk::HttpResponse result;
        result.status = response.status;
        if (response.body.size) result.body.assign(response.body.data, response.body.size);
        result.final_url = bounded(response.final_url, 4096);
        return result;
    }

private:
    ProwseTkAiOracleTransport transport_;
};

ai::OracleOptions convert(const ProwseTkAiOracleOptions& input) {
    if (input.timeout_ms > static_cast<uint32_t>(std::numeric_limits<int>::max())) invalid();
    ai::OracleOptions options;
    if (input.enabled != 0 && input.enabled != 1) invalid();
    options.enabled = input.enabled != 0;
    options.base_url = bounded(input.base_url, 4096);
    options.api_key = bounded(input.api_key, 4096);
    options.model = bounded(input.model, 256);
    options.organization = bounded(input.organization, 4096);
    options.project = bounded(input.project, 4096);
    options.timeout_ms = static_cast<int>(input.timeout_ms);
    options.max_input_bytes = input.max_input_bytes;
    options.max_response_bytes = input.max_response_bytes;
    options.max_requests = input.max_requests;
    options.max_output_tokens = input.max_output_tokens;
    return options;
}

int status(const prowsetk::Error& error) noexcept {
    if (error.code() == prowsetk::ErrorCode::SecurityViolation) return PROWSETK_STATUS_SECURITY_VIOLATION;
    if (error.code() == prowsetk::ErrorCode::InvalidArgument) return PROWSETK_STATUS_INVALID_ARGUMENT;
    return PROWSETK_STATUS_ERROR;
}
}  // namespace

struct ProwseTkAiOracle {
    CallbackNetwork network;
    ai::Oracle oracle;
    ai::OracleResult result;
    std::array<char, 192> error{};
    std::size_t max_input_bytes;

    ProwseTkAiOracle(ProwseTkAiOracleTransport transport, ai::OracleOptions options)
        : network(transport), oracle(network, options), max_input_bytes(options.max_input_bytes) {}
};

extern "C" {
void prowsetk_ai_oracle_options_init(ProwseTkAiOracleOptions* options) {
    if (!options) return;
    *options = {0, "https://api.openai.com/v1", nullptr, "gpt-4o-mini", nullptr, nullptr,
                30000, 256u * 1024u, 1024u * 1024u, 16, 512};
}

int prowsetk_ai_oracle_create(const ProwseTkAiOracleOptions* options,
                            const ProwseTkAiOracleTransport* transport, ProwseTkAiOracle** out) {
    if (!out) return PROWSETK_STATUS_INVALID_ARGUMENT;
    *out = nullptr;
    if (!options || !transport || !transport->send) return PROWSETK_STATUS_INVALID_ARGUMENT;
    try {
        auto oracle = std::make_unique<ProwseTkAiOracle>(*transport, convert(*options));
        *out = oracle.release();
        return PROWSETK_STATUS_OK;
    } catch (const prowsetk::Error& error) {
        return status(error);
    } catch (...) {
        return PROWSETK_STATUS_ERROR;
    }
}

void prowsetk_ai_oracle_free(ProwseTkAiOracle* oracle) {
    const std::unique_ptr<ProwseTkAiOracle> owner(oracle);
}

int prowsetk_ai_oracle_ask(ProwseTkAiOracle* oracle, const ProwseTkAiOracleRequest* request,
                         ProwseTkAiOracleResult* result) {
    if (result) *result = {};
    if (!oracle || !request || !result) return PROWSETK_STATUS_INVALID_ARGUMENT;
    try {
        oracle->result = {};
        oracle->error[0] = '\0';
        if (request->image_count > 4 || (request->image_count && !request->images) ||
            (request->json_output != 0 && request->json_output != 1)) invalid();
        ai::OracleRequest input;
        input.task = request->task ? ai::parse_task(bounded(request->task, 16)) : ai::Task::Advice;
        input.prompt = bounded(request->prompt, oracle->max_input_bytes);
        if (request->context_json) input.context_json = bounded(request->context_json, oracle->max_input_bytes);
        input.html = bounded(request->html, oracle->max_input_bytes);
        input.page_url = bounded(request->page_url, oracle->max_input_bytes);
        for (std::size_t i = 0; i < request->image_count; ++i) {
            input.images.push_back(bounded(request->images[i], oracle->max_input_bytes));
        }
        input.json_output = request->json_output != 0;
        oracle->result = oracle->oracle.ask(input);
        const auto& value = oracle->result;
        *result = {value.answer.c_str(), value.response_id.c_str(), value.model.c_str(),
                   value.provenance.c_str(), value.advisory ? 1 : 0, value.input_tokens, value.output_tokens};
        return PROWSETK_STATUS_OK;
    } catch (const prowsetk::Error& error) {
        std::snprintf(oracle->error.data(), oracle->error.size(), "%s", error.what());
        return status(error);
    } catch (...) {
        // Never return transport/JSON exception text across the ABI.
        std::snprintf(oracle->error.data(), oracle->error.size(), "%s", "ai-oracle: operation failed");
        return PROWSETK_STATUS_ERROR;
    }
}

const char* prowsetk_ai_oracle_error(const ProwseTkAiOracle* oracle) {
    return oracle ? oracle->error.data() : "ai-oracle: invalid handle";
}
}
