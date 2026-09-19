#include "prowsetk/plugins/captcha_handler.hpp"

#include <algorithm>
#include <cctype>
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

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char ch) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    });
    return value;
}

bool has_server_challenge_signal(const AntiBotDetection& detection) {
    for (const auto& signal : detection.signals) {
        const std::string source = lower(signal.source);
        const std::string evidence = lower(signal.name + " " + signal.detail);
        if (source == "header" ||
            evidence.find("recaptcha") != std::string::npos ||
            evidence.find("hcaptcha") != std::string::npos ||
            evidence.find("turnstile") != std::string::npos ||
            evidence.find("human-verification") != std::string::npos) {
            return true;
        }
    }
    return detection.status == 403 || detection.status == 429;
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
    result.diagnostic = result.detection.activated
                            ? "challenge inferred from rendered document"
                            : "no challenge evidence in rendered document";
    return result;
}

CaptchaHandlerResult inspect_response(const HttpRequest& request,
                                      const HttpResponse& response,
                                      const CaptchaHandlerOptions& options) {
    AntiBotDetector detector;
    CaptchaHandlerResult result;
    result.detection = detector.inspect_response(request, response);
    result.methods = available_methods(options);
    result.server_confirmed =
        result.detection.activated && has_server_challenge_signal(result.detection);
    if (!result.detection.activated) {
        result.diagnostic = "no challenge evidence in HTTP response";
    } else if (result.server_confirmed) {
        result.diagnostic = "server response contains anti-bot challenge evidence";
    } else {
        result.diagnostic =
            "challenge is heuristic and lacks a server confirmation signal";
    }
    return result;
}

HandlingMethod select_handling_method(const CaptchaHandlerResult& result,
                                      const HandlingInputs& inputs,
                                      const CaptchaHandlerOptions& options) {
    if (!result.detection.activated) return HandlingMethod::AbortAndReport;
    if (inputs.has_clearance_cookie && options.allow_cookie_session_reuse) {
        return HandlingMethod::CookieSessionReuse;
    }
    if (inputs.has_pre_solved_token && options.allow_pre_solved_token) {
        return HandlingMethod::PreSolvedToken;
    }
    if (result.detection.category == "rate-limit" &&
        options.allow_wait_for_clearance) {
        return HandlingMethod::WaitForClearance;
    }
    if (options.allow_manual_user_prompt) {
        return HandlingMethod::ManualUserPrompt;
    }
    return HandlingMethod::AbortAndReport;
}

}  // namespace prowsetk::plugins::captcha_handler
