#include "prowsetk/ProwseTk-Plugin.h"

#include <string.h>

static const char* const kCapabilities[] = {"document-extractor",
                                            "openapi-yaml",
                                            "network-hook"};

static const ProwseTkPluginInfo kInfo = {
    "test-plugin",
    "1.0.0",
    "2",
    "Plugin used by the ProwseTk test suite",
    PROWSETK_PLUGIN_NATIVE,
    "test_plugin_lua",
    "prowsetk-plugin",
    kCapabilities,
    3};

static int g_initialized = 0;
static int g_configured = 0;
static int g_before_count = 0;
static int g_after_count = 0;
static int g_document_count = 0;
static char g_last_redacted[128];

static int plugin_initialize(ProwseTkHost* host) {
    if (host == NULL || host->api == NULL) {
        return 1;
    }
    if (host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION) {
        return 2;
    }
    if (host->api->log != NULL) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "test-plugin initialized");
    }
    if (host->api->redact != NULL) {
        (void)host->api->redact(host->api->user_data,
                                "https://example.test/?token=secret&ok=yes",
                                g_last_redacted, sizeof(g_last_redacted));
    }
    g_initialized = 1;
    return 0;
}

static void plugin_shutdown(ProwseTkHost* host) {
    (void)host;
    g_initialized = 0;
}

static const ProwseTkPluginInfo* plugin_info(void) { return &kInfo; }

static int plugin_configure(ProwseTkHost* host,
                            const ProwseTkConfigEntry* entries,
                            size_t entry_count) {
    (void)host;
    for (size_t i = 0; i < entry_count; ++i) {
        if (entries[i].key != NULL && entries[i].value != NULL &&
            strcmp(entries[i].key, "mode") == 0 &&
            strcmp(entries[i].value, "strict") == 0) {
            g_configured = 1;
        }
    }
    return PROWSETK_STATUS_OK;
}

static int plugin_before_request(ProwseTkHost* host,
                                 const ProwseTkHttpRequest* request,
                                 ProwseTkHookResult* result) {
    (void)host;
    if (request == NULL || request->url == NULL || result == NULL) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    ++g_before_count;
    result->action = PROWSETK_HOOK_CONTINUE;
    result->message = NULL;
    result->replacement_request = NULL;
    if (strstr(request->url, "/blocked") != NULL) {
        result->action = PROWSETK_HOOK_REJECT;
        result->message = "blocked by test plugin";
        return PROWSETK_STATUS_OK;
    }
    if (strstr(request->url, "/replace") != NULL) {
        static const ProwseTkHeader headers[] = {{"X-Plugin", "rewritten"}};
        static const ProwseTkHttpRequest replacement = {
            "GET",
            "https://rewritten.example/final",
            headers,
            1,
            {NULL, 0},
            30000,
            33554432};
        result->action = PROWSETK_HOOK_REPLACE_REQUEST;
        result->replacement_request = &replacement;
    }
    return PROWSETK_STATUS_OK;
}

static int plugin_after_response(ProwseTkHost* host,
                                 const ProwseTkHttpRequest* request,
                                 const ProwseTkHttpResponse* response,
                                 ProwseTkHookResult* result) {
    (void)host;
    (void)request;
    if (response == NULL || result == NULL) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    ++g_after_count;
    result->action = PROWSETK_HOOK_CONTINUE;
    result->message = NULL;
    result->replacement_request = NULL;
    if (response->status == 451) {
        result->action = PROWSETK_HOOK_REJECT;
        result->message = "response rejected by test plugin";
    }
    return PROWSETK_STATUS_OK;
}

static int plugin_on_document(ProwseTkHost* host,
                              const ProwseTkDocumentSnapshot* document) {
    if (document == NULL || document->url == NULL || document->html == NULL) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    ++g_document_count;
    if (host != NULL && host->api != NULL && host->api->emit_event != NULL) {
        host->api->emit_event(host->api->user_data, "console", "test-plugin",
                              document->title);
    }
    return PROWSETK_STATUS_OK;
}

static const ProwseTkPlugin kPlugin = {
    plugin_initialize,
    plugin_shutdown,
    plugin_info,
    plugin_configure,
    plugin_before_request,
    plugin_after_response,
    plugin_on_document};

PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &kPlugin;
}

PROWSETK_PLUGIN_EXPORT int prowsetk_test_plugin_is_initialized(void) {
    return g_initialized;
}

PROWSETK_PLUGIN_EXPORT int prowsetk_test_plugin_configured(void) {
    return g_configured;
}

PROWSETK_PLUGIN_EXPORT int prowsetk_test_plugin_before_count(void) {
    return g_before_count;
}

PROWSETK_PLUGIN_EXPORT int prowsetk_test_plugin_after_count(void) {
    return g_after_count;
}

PROWSETK_PLUGIN_EXPORT int prowsetk_test_plugin_document_count(void) {
    return g_document_count;
}

PROWSETK_PLUGIN_EXPORT const char* prowsetk_test_plugin_last_redacted(void) {
    return g_last_redacted;
}
