#ifndef PROWSETK_PLUGINS_CAPTCHA_HANDLER_HPP
#define PROWSETK_PLUGINS_CAPTCHA_HANDLER_HPP

#include <string>
#include <vector>

#include "prowsetk/anti_bot_detection.hpp"
#include "prowsetk/document.hpp"

namespace prowsetk::plugins::captcha_handler {

enum class HandlingMethod {
    ManualUserPrompt,
    ExternalSolverApi,
    WebhookDispatch,
    LuaCallback,
    PreSolvedToken,
    CookieSessionReuse,
    WaitForClearance,
    AbortAndReport,
};

struct CaptchaHandlerOptions {
    bool allow_external_solver = false;
    bool allow_webhook = false;
    bool allow_lua_callback = false;
    bool allow_pre_solved_token = false;
    bool allow_cookie_session_reuse = false;
    bool allow_wait_for_clearance = true;
    bool allow_manual_user_prompt = true;
    bool redact_secrets = true;
};

struct HandlingPlan {
    HandlingMethod method = HandlingMethod::AbortAndReport;
    bool available = false;
    std::string name;
    std::string description;
    std::string configuration_key;
    std::string security_note;
};

struct CaptchaHandlerResult {
    AntiBotDetection detection;
    std::vector<HandlingPlan> methods;
};

const char* to_string(HandlingMethod method) noexcept;

std::vector<HandlingPlan> available_methods(
    const CaptchaHandlerOptions& options = {});

CaptchaHandlerResult inspect_document(
    const Document& document,
    const CaptchaHandlerOptions& options = {});

}  // namespace prowsetk::plugins::captcha_handler

#endif  // PROWSETK_PLUGINS_CAPTCHA_HANDLER_HPP
