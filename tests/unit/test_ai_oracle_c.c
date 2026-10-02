#include "prowsetk/plugins/ai_oracle.h"

#include <string.h>

static int calls;
static int send_response(void* data, const ProwseTkHttpRequest* request,
                         ProwseTkHttpResponse* response) {
    static const char body[] =
        "{\"id\":\"resp_c\",\"model\":\"fixture-model\",\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"status\":\"completed\","
        "\"content\":[{\"type\":\"output_text\",\"text\":\"inspect catalog\"}]}]}";
    if (data != &calls || strcmp(request->method, "POST") ||
        strcmp(request->url, "https://api.openai.com/v1/responses") ||
        request->max_response_bytes == 0 || request->timeout_ms == 0) return PROWSETK_STATUS_ERROR;
    ++calls;
    response->status = 200;
    response->body.data = body;
    response->body.size = sizeof(body) - 1;
    return PROWSETK_STATUS_OK;
}

int test_ai_oracle_c_contract(void) {
    ProwseTkAiOracleOptions options;
    ProwseTkAiOracle* oracle = NULL;
    ProwseTkAiOracleRequest request = {0};
    ProwseTkAiOracleResult result = {0};
    ProwseTkAiOracleTransport transport = {&calls, send_response};
    int ok;
    calls = 0;
    prowsetk_ai_oracle_options_init(&options);
    options.api_key = "fixture-key";
    request.task = "crawl";
    request.prompt = "Inspect the next link";
    if (prowsetk_ai_oracle_create(&options, &transport, &oracle) != PROWSETK_STATUS_OK) return 1;
    ok = prowsetk_ai_oracle_ask(oracle, &request, &result) == PROWSETK_STATUS_SECURITY_VIOLATION &&
         result.answer == NULL && calls == 0;
    prowsetk_ai_oracle_free(oracle);
    if (!ok) return 2;
    options.enabled = 1;
    options.max_requests = 1;
    if (prowsetk_ai_oracle_create(&options, &transport, &oracle) != PROWSETK_STATUS_OK) return 3;
    ok = prowsetk_ai_oracle_ask(oracle, &request, &result) == PROWSETK_STATUS_OK &&
         !strcmp(result.answer, "inspect catalog") && result.advisory == 1 && calls == 1;
    if (!ok) { prowsetk_ai_oracle_free(oracle); return 4; }
    ok = prowsetk_ai_oracle_ask(oracle, &request, &result) == PROWSETK_STATUS_ERROR &&
         result.answer == NULL && calls == 1 &&
         strstr(prowsetk_ai_oracle_error(oracle), "budget") != NULL;
    prowsetk_ai_oracle_free(oracle);
    prowsetk_ai_oracle_free(NULL);
    if (!ok) return 5;
    oracle = NULL;
    if (prowsetk_ai_oracle_create(NULL, &transport, &oracle) != PROWSETK_STATUS_INVALID_ARGUMENT || oracle) return 6;
    return 0;
}
