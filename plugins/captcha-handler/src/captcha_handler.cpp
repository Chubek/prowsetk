#include "prowsetk/plugins/captcha_handler.hpp"

#include <utility>

namespace prowsetk::plugins::captcha_handler {
namespace {

HandlingPlan plan(HandlingMethod method, bool available, std::string name,
                  std::string description, std::string configuration_key,
                  std::string security_note) {
    return HandlingPlan{method,
                        available,
                        std::move(name),
                        std::move(description),
                        std::move(configuration_key),
                        std::move(security_note)};
}

}  // namespace

const char* to_string(HandlingMethod method) noexcept {
    switch (method) {
        case HandlingMethod::ManualUserPrompt: return "manual-user-prompt";
        case HandlingMethod::ExternalSolverApi: return "external-solver-api";
        case HandlingMethod::WebhookDispatch: return "webhook-dispatch";
        case HandlingMethod::LuaCallback: return "lua-callback";
        case HandlingMethod::PreSolvedToken: return "pre-solved-token";
        case HandlingMethod::CookieSessionReuse: return "cookie-session-reuse";
        case HandlingMethod::WaitForClearance: return "wait-for-clearance";
        case HandlingMethod::AbortAndReport: return "abort-and-report";
    }
    return "abort-and-report";
}

std::vector<HandlingPlan> available_methods(
    const CaptchaHandlerOptions& options) {
    return {
        plan(HandlingMethod::ManualUserPrompt, options.allow_manual_user_prompt,
             "Manual user prompt",
             "pause automation and ask the embedding application to obtain a token",
             "manual_prompt", "challenge content is not logged"),
        plan(HandlingMethod::ExternalSolverApi, options.allow_external_solver,
             "External solver API",
             "delegate challenge metadata to a configured solver service",
             "solver_api", "API keys and returned tokens must be redacted"),
        plan(HandlingMethod::WebhookDispatch, options.allow_webhook,
             "Webhook dispatch",
             "send a host-mediated webhook for out-of-process solving",
             "webhook_url", "webhook payloads omit cookies by default"),
        plan(HandlingMethod::LuaCallback, options.allow_lua_callback,
             "Lua callback",
             "call a Lua extension callback with heuristic challenge metadata",
             "lua_callback", "Lua receives no raw native handles"),
        plan(HandlingMethod::PreSolvedToken, options.allow_pre_solved_token,
             "Pre-solved token",
             "inject a token supplied by the caller before retrying",
             "pre_solved_token", "token values are treated as secrets"),
        plan(HandlingMethod::CookieSessionReuse,
             options.allow_cookie_session_reuse, "Cookie session reuse",
             "reuse a caller-provided clearance cookie or session profile",
             "clearance_cookie", "cookies remain under the storage policy"),
        plan(HandlingMethod::WaitForClearance, options.allow_wait_for_clearance,
             "Wait for clearance",
             "retry later when a temporary WAF or rate-limit challenge clears",
             "retry_policy", "bounded retries only; no wall-clock sleeps in tests"),
        plan(HandlingMethod::AbortAndReport, true, "Abort and report",
             "stop the workflow and return challenge provenance to the caller",
             "abort", "default fallback when no solver is configured"),
    };
}

CaptchaHandlerResult inspect_document(const Document& document,
                                      const CaptchaHandlerOptions& options) {
    AntiBotDetector detector;
    CaptchaHandlerResult result;
    result.detection = detector.inspect_document(document);
    result.methods = available_methods(options);
    return result;
}

}  // namespace prowsetk::plugins::captcha_handler
