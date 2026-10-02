# Chapter 18: Anti-bot and captcha-handler

[Manual index](README.md)

## Detection

AntiBotDetector inspects HTTP responses and DOM documents for challenge,
rate-limit, and bot-protection signals. It returns AntiBotDetection with
`activated`, `category`, URL/status, confidence, and signal records containing
source/name/detail/confidence.

```cpp
prowsetk::AntiBotDetector detector;
const auto detection = detector.inspect_document(*session->document());
if (detection.activated) {
    // Choose a handling policy using category, confidence, and provenance.
}
```

Session maintains the latest optional `anti_bot_detection()` and emits
`anti_bot_detected` events. Lua can subscribe to that event and use the plugin's
response helper. The current generic Lua session method table does not expose
a separate `detect_anti_bot()` call.

HTTP 403/429, WAF headers, challenge titles/text, and reCAPTCHA/hCaptcha/Turnstile
markers provide heuristic evidence. A signal is not authoritative proof of the
vendor, cause, or successful clearance.

## captcha-handler plans

The native plugin builds on detection and reports allowed handling plans. C++
`available_methods`, `inspect_document`, `inspect_response`, and
`select_handling_method` operate on policy options and presence flags.

| Method | Host responsibility |
|---|---|
| `manual-user-prompt` | Obtain human assistance and fresh session evidence |
| `external-solver-api` | Configure an allowed solver and host-mediated request |
| `webhook-dispatch` | Configure an allowed notification/service workflow |
| `lua-callback` | Invoke a trusted configured automation callback |
| `pre-solved-token` | Use caller-supplied secret challenge material appropriately |
| `cookie-session-reuse` | Import/reuse caller-supplied session clearance |
| `wait-for-clearance` | Apply a bounded wait/retry policy |
| `abort-and-report` | Stop with a sanitized diagnostic |

Default C++ CaptchaHandlerOptions allow manual prompt and wait-for-clearance;
external solver/webhook/Lua callback/pre-solved token/cookie reuse require
explicit enablement. Redaction defaults true. These APIs describe handling
plans; they do not implement an automatic challenge-solving service.

## Lua response helper

```lua
local captcha = dofile('plugins/captcha-handler/lua/captcha_handler.lua')
local response = session:request('GET', 'https://example.com/')
local result = captcha.inspect_response('GET', 'https://example.com/', response)
local method = captcha.select_handling_method(result, {
    has_clearance_cookie = false,
    has_pre_solved_token = false
}, { allow_manual_user_prompt = true })
```

`inspect_response` examines status/headers/body and returns signals, category,
confidence, activation, a sanitized diagnostic, and `server_confirmed`.
That last flag means concrete response evidence was found; it does not mean
that the challenge was solved or the user is logged in.

Lua method selection uses input-presence booleans rather than returning secret
values. Its cookie/token presence defaults differ from the C++ options:
disable them explicitly when they are unavailable to your policy. The Lua
helper is supplied in the source tree; its CMake target installs the native
plugin/header, so arrange deployment of the Lua file when using it outside
the checkout.

## Confirmed-session workflow

1. Inspect actual HTTP/DOM evidence and record its sanitized provenance.
2. Choose the enabled handling method in the host.
3. Obtain new cookies/token data or a browser-approved page through the selected
   workflow.
4. Reload in a fresh or appropriately updated session.
5. Require successful HTTP and authenticated-only DOM evidence before exporting
   protected results.

Crawler can supervise an approved assistant command/project and retry once.
Booking examples can import fresh Firefox cookies. Beacon transfers data from
an explicitly connected Firefox tab. These mechanisms have separate data and
consent contracts; successful process completion or cookie presence is not
authentication evidence.

Reference: `anti_bot_detection.hpp`,
`plugins/captcha-handler/include/prowsetk/plugins/captcha_handler.hpp`,
`plugins/captcha-handler/lua/captcha_handler.lua`.

**Next:** [restful-resolver](19-restful-resolver.md).
