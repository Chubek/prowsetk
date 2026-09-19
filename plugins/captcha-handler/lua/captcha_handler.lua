-- Lua-facing policy helper for plugins/captcha-handler.
--
-- This module mirrors the native plugin's host-mediated posture: it classifies
-- response evidence, selects an allowed handling plan, and never logs or
-- returns secret-bearing cookie/token values.
local M = {_version = "0.1.0"}

local function lower(value)
    return tostring(value or ""):lower()
end

local function header(headers, name)
    name = lower(name)
    for key, value in pairs(headers or {}) do
        if lower(key) == name then return tostring(value or "") end
    end
    return nil
end

local function add_signal(result, source, name, detail, confidence)
    result.signals[#result.signals + 1] = {
        source = source,
        name = name,
        detail = detail,
        confidence = confidence
    }
    if confidence > result.confidence then result.confidence = confidence end
    result.activated = result.confidence >= 0.55
end

local function classify(result)
    local captcha, rate_limit, waf = false, false, false
    for _, signal in ipairs(result.signals) do
        local text = lower((signal.name or "") .. " " .. (signal.detail or ""))
        captcha = captcha or text:find("captcha", 1, true) ~= nil or
            text:find("recaptcha", 1, true) ~= nil or
            text:find("hcaptcha", 1, true) ~= nil or
            text:find("turnstile", 1, true) ~= nil or
            text:find("human%-verification") ~= nil
        rate_limit = rate_limit or text:find("rate limit", 1, true) ~= nil or
            text:find("too many requests", 1, true) ~= nil
        waf = waf or text:find("cloudflare", 1, true) ~= nil or
            text:find("datadome", 1, true) ~= nil or
            text:find("distil", 1, true) ~= nil
    end
    if captcha then
        result.category = "captcha"
    elseif rate_limit then
        result.category = "rate-limit"
    elseif waf then
        result.category = "waf-challenge"
    elseif #result.signals > 0 then
        result.category = "anti-bot"
    end
end

function M.inspect_response(method, url, response)
    local result = {
        activated = false,
        category = "",
        url = (response and response.final_url ~= "" and response.final_url) or url or "",
        status = tonumber(response and response.status) or 0,
        confidence = 0,
        signals = {},
        server_confirmed = false,
        diagnostic = "no challenge evidence in HTTP response"
    }
    local body = lower(response and response.body or "")
    local headers = (response and response.headers) or {}

    if result.status == 403 then
        add_signal(result, "http", "forbidden",
            "HTTP 403 can indicate a WAF or bot challenge", 0.55)
    elseif result.status == 429 then
        add_signal(result, "http", "rate-limit", "HTTP 429 Too Many Requests", 0.70)
    elseif result.status == 503 then
        add_signal(result, "http", "service-unavailable",
            "HTTP 503 can indicate a temporary challenge page", 0.45)
    end

    if lower(header(headers, "cf-mitigated")) == "challenge" then
        add_signal(result, "header", "cloudflare-challenge",
            "cf-mitigated header reports challenge", 0.95)
    end
    if header(headers, "x-datadome") or header(headers, "x-distil-cs") then
        add_signal(result, "header", "bot-protection-header",
            "bot protection response header", 0.85)
    end

    if body:find("g%-recaptcha") or body:find("google.com/recaptcha", 1, true) then
        add_signal(result, "response-body", "recaptcha", "Google reCAPTCHA marker", 0.90)
    end
    if body:find("h%-captcha") or body:find("hcaptcha.com", 1, true) then
        add_signal(result, "response-body", "hcaptcha", "hCaptcha marker", 0.90)
    end
    if body:find("cf%-turnstile") or body:find("challenges.cloudflare.com", 1, true) then
        add_signal(result, "response-body", "turnstile", "Cloudflare Turnstile marker", 0.90)
    end
    if body:find("human verification", 1, true) or
       body:find("verify you are human", 1, true) or
       body:find("verification required", 1, true) then
        add_signal(result, "response-body", "human-verification",
            "human verification challenge text", 0.75)
    elseif body:find("captcha", 1, true) then
        add_signal(result, "response-body", "captcha-text",
            "page text mentions captcha", 0.70)
    end

    classify(result)
    if result.activated then
        for _, signal in ipairs(result.signals) do
            local evidence = lower((signal.name or "") .. " " .. (signal.detail or ""))
            if signal.source == "header" or result.status == 403 or result.status == 429 or
               evidence:find("recaptcha", 1, true) or
               evidence:find("hcaptcha", 1, true) or
               evidence:find("turnstile", 1, true) or
               evidence:find("human%-verification") then
                result.server_confirmed = true
                break
            end
        end
        result.diagnostic = result.server_confirmed and
            "server response contains anti-bot challenge evidence" or
            "challenge is heuristic and lacks a server confirmation signal"
    end
    return result
end

function M.select_handling_method(result, inputs, options)
    inputs = inputs or {}
    options = options or {}
    if not result or not result.activated then return "abort-and-report" end
    if inputs.has_clearance_cookie and options.allow_cookie_session_reuse ~= false then
        return "cookie-session-reuse"
    end
    if inputs.has_pre_solved_token and options.allow_pre_solved_token ~= false then
        return "pre-solved-token"
    end
    if result.category == "rate-limit" and options.allow_wait_for_clearance ~= false then
        return "wait-for-clearance"
    end
    if options.allow_manual_user_prompt then return "manual-user-prompt" end
    return "abort-and-report"
end

return M
