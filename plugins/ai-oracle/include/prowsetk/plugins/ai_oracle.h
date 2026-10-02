#ifndef PROWSETK_PLUGINS_AI_ORACLE_H
#define PROWSETK_PLUGINS_AI_ORACLE_H

#include "prowsetk/ProwseTk-Plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROWSETK_AI_ORACLE_API_VERSION 1

typedef struct ProwseTkAiOracle ProwseTkAiOracle;

typedef struct {
    int enabled;
    const char* base_url;
    const char* api_key;
    const char* model;
    const char* organization;
    const char* project;
    uint32_t timeout_ms;
    size_t max_input_bytes;
    size_t max_response_bytes;
    size_t max_requests;
    uint32_t max_output_tokens;
} ProwseTkAiOracleOptions;

/* Initialize defaults (disabled, no key). String fields are borrowed on create,
 * copied into the handle, and never logged. Configuration is per handle. */
PROWSETK_PLUGIN_EXPORT void prowsetk_ai_oracle_options_init(ProwseTkAiOracleOptions* options);

typedef struct {
    void* user_data;
    /* The host mediates network access. Do not follow redirects. Honor the
     * request's timeout/response bounds. Response pointers belong to the host
     * and must remain valid until the next send or handle destruction. They
     * are copied immediately; ai-oracle never frees them. */
    int (*send)(void* user_data, const ProwseTkHttpRequest* request,
                ProwseTkHttpResponse* response);
} ProwseTkAiOracleTransport;

typedef struct {
    const char* task; /* "advice", "captcha", or "crawl"; NULL means advice */
    const char* prompt;
    const char* context_json; /* object; NULL means {} */
    const char* html;
    const char* page_url;
    const char* const* images; /* embedded PNG/JPEG data URLs; at most four */
    size_t image_count;
    int json_output;
} ProwseTkAiOracleRequest;

typedef struct {
    const char* answer;
    const char* response_id;
    const char* model;
    const char* provenance;
    int advisory;
    uint64_t input_tokens;
    uint64_t output_tokens;
} ProwseTkAiOracleResult;

/* Status codes are ProwseTkStatus. Create sets *out to NULL on failure.
 * Serialize access to a handle. The transport/user_data must outlive it. */
PROWSETK_PLUGIN_EXPORT int prowsetk_ai_oracle_create(
    const ProwseTkAiOracleOptions* options,
    const ProwseTkAiOracleTransport* transport, ProwseTkAiOracle** out);
PROWSETK_PLUGIN_EXPORT void prowsetk_ai_oracle_free(ProwseTkAiOracle* oracle);

/* Result strings belong to the handle and expire on the next ask/free.
 * A failed ask clears the result; no exception crosses this C boundary. */
PROWSETK_PLUGIN_EXPORT int prowsetk_ai_oracle_ask(
    ProwseTkAiOracle* oracle, const ProwseTkAiOracleRequest* request,
    ProwseTkAiOracleResult* result);

/* Borrowed, fixed secret-free error message, valid until next ask/free. */
PROWSETK_PLUGIN_EXPORT const char* prowsetk_ai_oracle_error(const ProwseTkAiOracle* oracle);

#ifdef __cplusplus
}
#endif

#endif /* PROWSETK_PLUGINS_AI_ORACLE_H */
