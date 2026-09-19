#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/document.hpp"
#include "prowsetk/plugins/captcha_handler.hpp"

#include <cstring>
#include <string>

namespace {

namespace captcha = prowsetk::plugins::captcha_handler;

const char* const kCapabilities[] = {
    "captcha-handling",
    "anti-bot-detection",
    "manual-user-prompt",
    "external-solver-api",
    "webhook-dispatch",
    "lua-callback",
    "pre-solved-token",
    "cookie-session-reuse",
    "wait-for-clearance",
    "abort-and-report",
};

const ProwseTkPluginInfo kInfo = {
    "captcha-handler",
    "0.1.0",
    "2",
    "Detect anti-bot challenges and plan host-mediated CAPTCHA handling methods",
    PROWSETK_PLUGIN_NATIVE,
    "captcha_handler",
    "prowsetk-plugin",
    kCapabilities,
    10,
};

captcha::CaptchaHandlerOptions g_options;

bool truthy(const char* value) {
    return value != nullptr &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "yes") == 0 || std::strcmp(value, "on") == 0);
}

int plugin_initialize(ProwseTkHost* host) {
    if (host == nullptr || host->api == nullptr) return PROWSETK_STATUS_ERROR;
    if (host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION) {
        return PROWSETK_STATUS_ERROR;
    }
    if (host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "captcha-handler plugin initialized");
    }
    return PROWSETK_STATUS_OK;
}

void plugin_shutdown(ProwseTkHost* host) {
    if (host != nullptr && host->api != nullptr && host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                       "captcha-handler plugin shutdown");
    }
}

const ProwseTkPluginInfo* plugin_info() { return &kInfo; }

int plugin_configure(ProwseTkHost*, const ProwseTkConfigEntry* entries,
                     std::size_t entry_count) {
    if (entries == nullptr && entry_count != 0) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    for (std::size_t i = 0; i < entry_count; ++i) {
        const ProwseTkConfigEntry& entry = entries[i];
        if (entry.key == nullptr) continue;
        const std::string key = entry.key;
        if (key == "captcha.manual_prompt") {
            g_options.allow_manual_user_prompt = truthy(entry.value);
        } else if (key == "captcha.solver_api") {
            g_options.allow_external_solver = truthy(entry.value);
        } else if (key == "captcha.webhook") {
            g_options.allow_webhook = truthy(entry.value);
        } else if (key == "captcha.lua_callback") {
            g_options.allow_lua_callback = truthy(entry.value);
        } else if (key == "captcha.pre_solved_token") {
            g_options.allow_pre_solved_token = truthy(entry.value);
        } else if (key == "captcha.cookie_session_reuse") {
            g_options.allow_cookie_session_reuse = truthy(entry.value);
        } else if (key == "captcha.wait_for_clearance") {
            g_options.allow_wait_for_clearance = truthy(entry.value);
        }
    }
    return PROWSETK_STATUS_OK;
}

int plugin_on_document(ProwseTkHost* host,
                       const ProwseTkDocumentSnapshot* snapshot) {
    if (host == nullptr || host->api == nullptr || snapshot == nullptr) {
        return PROWSETK_STATUS_INVALID_ARGUMENT;
    }
    const std::string html = snapshot->html != nullptr ? snapshot->html : "";
    const std::string url = snapshot->url != nullptr ? snapshot->url : "";
    const auto document = prowsetk::parse_html(html, url, url);
    const auto result = captcha::inspect_document(*document, g_options);
    if (!result.detection.activated) {
        return PROWSETK_STATUS_OK;
    }
    if (host->api->emit_event != nullptr) {
        host->api->emit_event(host->api->user_data, "anti_bot_detected",
                              "captcha-handler",
                              result.detection.category.c_str());
    }
    if (host->api->log != nullptr) {
        host->api->log(host->api->user_data, PROWSETK_LOG_WARN,
                       "captcha-handler detected anti-bot challenge");
    }
    return PROWSETK_STATUS_OK;
}

const ProwseTkPlugin kPlugin = {plugin_initialize,
                                plugin_shutdown,
                                plugin_info,
                                plugin_configure,
                                nullptr,
                                nullptr,
                                plugin_on_document};

}  // namespace

extern "C" {
PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
    return &kPlugin;
}
}
