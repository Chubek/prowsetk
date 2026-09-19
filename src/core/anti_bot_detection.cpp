#include "prowsetk/anti_bot_detection.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace prowsetk {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return value;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

void add_signal(AntiBotDetection& detection, std::string source,
                std::string name, std::string detail, double confidence) {
    detection.signals.push_back(AntiBotSignal{
        std::move(source), std::move(name), std::move(detail), confidence});
    detection.confidence = std::max(detection.confidence, confidence);
    detection.activated = detection.confidence >= 0.55;
}

void classify(AntiBotDetection& detection) {
    bool captcha = false;
    bool rate_limit = false;
    bool waf = false;
    for (const auto& signal : detection.signals) {
        const std::string name = lower(signal.name + " " + signal.detail);
        captcha = captcha || contains(name, "captcha") ||
                  contains(name, "recaptcha") || contains(name, "hcaptcha") ||
                  contains(name, "turnstile") ||
                  contains(name, "human-verification");
        rate_limit = rate_limit || contains(name, "rate limit") ||
                     contains(name, "too many requests");
        waf = waf || contains(name, "cloudflare") || contains(name, "akamai") ||
              contains(name, "imperva") || contains(name, "distil") ||
              contains(name, "datadome");
    }
    if (captcha) {
        detection.category = "captcha";
    } else if (rate_limit) {
        detection.category = "rate-limit";
    } else if (waf) {
        detection.category = "waf-challenge";
    } else if (!detection.signals.empty()) {
        detection.category = "anti-bot";
    }
}

void inspect_text(AntiBotDetection& detection, std::string_view source,
                  const std::string& raw) {
    const std::string text = lower(raw);
    if (contains(text, "g-recaptcha") || contains(text, "google.com/recaptcha")) {
        add_signal(detection, std::string(source), "recaptcha",
                   "Google reCAPTCHA marker", 0.9);
    }
    if (contains(text, "h-captcha") || contains(text, "hcaptcha.com")) {
        add_signal(detection, std::string(source), "hcaptcha",
                   "hCaptcha marker", 0.9);
    }
    if (contains(text, "cf-turnstile") || contains(text, "challenges.cloudflare.com")) {
        add_signal(detection, std::string(source), "turnstile",
                   "Cloudflare Turnstile marker", 0.9);
    }
    if (contains(text, "captcha")) {
        add_signal(detection, std::string(source), "captcha-text",
                   "page text mentions captcha", 0.7);
    }
    if (contains(text, "verify you are human") ||
        contains(text, "are you a human") ||
        contains(text, "unusual traffic")) {
        add_signal(detection, std::string(source), "human-verification",
                   "human verification challenge text", 0.75);
    }
    if (contains(text, "access denied") &&
        (contains(text, "bot") || contains(text, "automated"))) {
        add_signal(detection, std::string(source), "access-denied-bot",
                   "access denied text references automation", 0.7);
    }
}

}  // namespace

AntiBotDetection AntiBotDetector::inspect_response(
    const HttpRequest& request, const HttpResponse& response) const {
    AntiBotDetection detection;
    detection.url = response.final_url.empty() ? request.url : response.final_url;
    detection.status = response.status;

    if (response.status == 403) {
        add_signal(detection, "http", "forbidden",
                   "HTTP 403 can indicate a WAF or bot challenge", 0.55);
    } else if (response.status == 429) {
        add_signal(detection, "http", "rate-limit",
                   "HTTP 429 Too Many Requests", 0.7);
    } else if (response.status == 503) {
        add_signal(detection, "http", "service-unavailable",
                   "HTTP 503 can indicate a temporary challenge page", 0.45);
    }

    for (const auto& [name, value] : response.headers) {
        const std::string header_name = lower(name);
        const std::string header_value = lower(value);
        if (header_name == "cf-mitigated" && header_value == "challenge") {
            add_signal(detection, "header", "cloudflare-challenge",
                       "cf-mitigated header reports challenge", 0.95);
        }
        if (header_name == "server" && contains(header_value, "cloudflare")) {
            add_signal(detection, "header", "cloudflare-server",
                       "Cloudflare server header", 0.35);
        }
        if (header_name == "x-datadome" || header_name == "x-distil-cs") {
            add_signal(detection, "header", "bot-protection-header", name,
                       0.85);
        }
    }

    inspect_text(detection, "response-body", response.body);
    classify(detection);
    return detection;
}

AntiBotDetection AntiBotDetector::inspect_document(
    const Document& document) const {
    AntiBotDetection detection;
    detection.url = document.url();
    detection.status = 0;
    inspect_text(detection, "document", document.title());
    inspect_text(detection, "document", document.text());
    inspect_text(detection, "document", document.html());

    if (document.get_element_by_id("captcha") != nullptr ||
        document.get_element_by_id("recaptcha") != nullptr ||
        document.get_element_by_id("hcaptcha") != nullptr) {
        add_signal(detection, "document", "captcha-element-id",
                   "captcha-like element id", 0.8);
    }
    if (!document.query_selector_all("[name*=captcha], [id*=captcha], [class*=captcha]").empty()) {
        add_signal(detection, "document", "captcha-selector",
                   "captcha-like form field or element", 0.8);
    }

    classify(detection);
    return detection;
}

AntiBotDetection AntiBotDetector::merge(AntiBotDetection first,
                                        const AntiBotDetection& second) const {
    if (first.url.empty()) first.url = second.url;
    if (first.status == 0) first.status = second.status;
    first.signals.insert(first.signals.end(), second.signals.begin(),
                         second.signals.end());
    first.confidence = std::max(first.confidence, second.confidence);
    first.activated = first.activated || second.activated;
    if (first.category.empty()) {
        first.category = second.category;
    } else if (first.category != "captcha" && second.category == "captcha") {
        first.category = second.category;
    }
    classify(first);
    return first;
}

const char* to_string(const AntiBotDetection& detection) noexcept {
    return detection.activated ? "activated" : "not_detected";
}

}  // namespace prowsetk
