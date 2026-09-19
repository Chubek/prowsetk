#include "prowsetk/javascript_runtime.hpp"

#ifdef PROWSETK_HAVE_QUICKJS

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <quickjs.h>

#include "prowsetk/url.hpp"
#include "prowsetk/error.hpp"
#include "core/web_platform_shim.hpp"

namespace prowsetk {
namespace {

class QuickJavaScriptRuntime;

struct RuntimeDeleter {
    void operator()(JSRuntime* runtime) const noexcept {
        if (runtime != nullptr) JS_FreeRuntime(runtime);
    }
};

struct ContextDeleter {
    void operator()(JSContext* context) const noexcept {
        if (context != nullptr) JS_FreeContext(context);
    }
};

using RuntimePtr = std::unique_ptr<JSRuntime, RuntimeDeleter>;
using ContextPtr = std::unique_ptr<JSContext, ContextDeleter>;

struct Deadline {
    std::chrono::steady_clock::time_point expires_at;
    bool expired = false;
};

int interrupt_at_deadline(JSRuntime*, void* opaque) {
    auto* deadline = static_cast<Deadline*>(opaque);
    deadline->expired = deadline->expired ||
                        std::chrono::steady_clock::now() >= deadline->expires_at;
    return deadline->expired ? 1 : 0;
}

std::string validate_options(const ScriptOptions& options) {
    if (options.timeout_ms <= 0) return "JavaScript timeout must be positive";
    if (options.memory_limit_bytes == 0) return "JavaScript memory limit must be positive";
    if (options.max_microtask_jobs == 0) return "JavaScript microtask limit must be positive";
    return {};
}

std::string value_to_string(JSContext* context, JSValueConst value) {
    std::size_t length = 0;
    const char* text = JS_ToCStringLen(context, &length, value);
    if (text == nullptr) return {};
    std::string result(text, length);
    JS_FreeCString(context, text);
    return result;
}

std::string take_exception(JSContext* context) {
    JSValue exception = JS_GetException(context);
    std::string message = value_to_string(context, exception);
    if (JS_IsError(exception)) {
        JSValue stack = JS_GetPropertyStr(context, exception, "stack");
        if (!JS_IsUndefined(stack)) {
            std::string stack_text = value_to_string(context, stack);
            if (!stack_text.empty()) {
                if (!message.empty()) message.push_back('\n');
                message += stack_text;
            }
        }
        JS_FreeValue(context, stack);
    }
    JS_FreeValue(context, exception);
    if (JS_HasException(context)) {
        JS_FreeValue(context, JS_GetException(context));
    }
    return message.empty() ? "JavaScript evaluation failed" : message;
}

int install_console_binding(JSContext* context);
JSValue queue_microtask(JSContext* context, JSValueConst, int argc,
                        JSValueConst* argv);
void promise_rejection_tracker(JSContext* context, JSValueConst,
                               JSValueConst reason, bool is_handled, void*);

// ---- Binding argument helpers ----------------------------------------------

QuickJavaScriptRuntime* runtime_of(JSContext* context);
DocumentScriptHost* host_of(JSContext* context);

bool arg_string(JSContext* context, JSValueConst value, std::string& out) {
    if (JS_IsUndefined(value) || JS_IsNull(value)) return false;
    out = value_to_string(context, value);
    return !JS_HasException(context);
}

ElementHandle arg_handle(JSContext* context, JSValueConst value) {
    std::int64_t number = 0;
    JS_ToInt64(context, &number, value);
    return static_cast<ElementHandle>(number);
}

JSValue handles_to_array(JSContext* context,
                         const std::vector<ElementHandle>& handles) {
    JSValue array = JS_NewArray(context);
    for (std::size_t i = 0; i < handles.size(); ++i) {
        JS_SetPropertyUint32(context, array, static_cast<std::uint32_t>(i),
                             JS_NewInt64(context,
                                         static_cast<std::int64_t>(handles[i])));
    }
    return array;
}

JSValue string_property(JSContext* context, JSValueConst object,
                        const char* name) {
    JSValue value = JS_GetPropertyStr(context, object, name);
    std::string text = value_to_string(context, value);
    JS_FreeValue(context, value);
    return JS_NewString(context, text.c_str());
}

// Reads a [[name, value], ...] array into header pairs.
std::vector<std::pair<std::string, std::string>> arg_header_pairs(
    JSContext* context, JSValueConst value) {
    std::vector<std::pair<std::string, std::string>> headers;
    if (!JS_IsArray(value)) return headers;
    const auto length = JS_GetPropertyStr(context, value, "length");
    std::int64_t count = 0;
    JS_ToInt64(context, &count, length);
    JS_FreeValue(context, length);
    for (std::int64_t i = 0; i < count; ++i) {
        JSValue pair = JS_GetPropertyUint32(context, value,
                                            static_cast<std::uint32_t>(i));
        if (JS_IsArray(pair)) {
            JSValue key = JS_GetPropertyUint32(context, pair, 0);
            JSValue val = JS_GetPropertyUint32(context, pair, 1);
            std::string name = value_to_string(context, key);
            std::string text = value_to_string(context, val);
            if (!name.empty()) headers.emplace_back(std::move(name), std::move(text));
            JS_FreeValue(context, key);
            JS_FreeValue(context, val);
        }
        JS_FreeValue(context, pair);
    }
    return headers;
}

JSValue pairs_to_array(JSContext* context,
                       const std::vector<std::pair<std::string, std::string>>& pairs) {
    JSValue array = JS_NewArray(context);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        JSValue pair = JS_NewArray(context);
        JS_SetPropertyUint32(context, pair, 0,
                             JS_NewString(context, pairs[i].first.c_str()));
        JS_SetPropertyUint32(context, pair, 1,
                             JS_NewString(context, pairs[i].second.c_str()));
        JS_SetPropertyUint32(context, array, static_cast<std::uint32_t>(i), pair);
    }
    return array;
}

// ---- Web platform bindings --------------------------------------------------
// Every binding resolves the host through the context opaque slot. A missing
// host (runtime created before navigation) degrades to empty values instead
// of throwing so page scripts keep running deterministically.

JSValue bp_page_info(JSContext* context, JSValueConst, int, JSValueConst*) {
    auto* host = host_of(context);
    const PageInfo info = host == nullptr ? PageInfo{} : host->page_info();
    JSValue object = JS_NewObject(context);
    JS_SetPropertyStr(context, object, "url",
                      JS_NewString(context, info.url.c_str()));
    JS_SetPropertyStr(context, object, "referrer",
                      JS_NewString(context, info.referrer.c_str()));
    JS_SetPropertyStr(context, object, "title",
                      JS_NewString(context, info.title.c_str()));
    return object;
}

JSValue bp_set_title(JSContext* context, JSValueConst, int argc,
                     JSValueConst* argv) {
    auto* host = host_of(context);
    std::string title;
    if (host != nullptr && argc > 0 && arg_string(context, argv[0], title)) {
        host->set_document_title(title);
    }
    return JS_UNDEFINED;
}

JSValue bp_navigator_info(JSContext* context, JSValueConst, int, JSValueConst*) {
    auto* host = host_of(context);
    const NavigatorInfo info =
        host == nullptr ? NavigatorInfo{} : host->navigator_info();
    JSValue object = JS_NewObject(context);
    JS_SetPropertyStr(context, object, "userAgent",
                      JS_NewString(context, info.user_agent.c_str()));
    JS_SetPropertyStr(context, object, "platform",
                      JS_NewString(context, info.platform.c_str()));
    JS_SetPropertyStr(context, object, "language",
                      JS_NewString(context, info.language.c_str()));
    JS_SetPropertyStr(context, object, "cookieEnabled",
                      JS_NewBool(context, info.cookie_enabled));
    JS_SetPropertyStr(context, object, "onLine", JS_NewBool(context, info.on_line));
    return object;
}

JSValue bp_request(JSContext* context, JSValueConst, int argc,
                   JSValueConst* argv);

// Defined after QuickJavaScriptRuntime: needs the complete type for deadline
// extension around the host-mediated network call.

JSValue bp_navigate(JSContext* context, JSValueConst, int argc,
                    JSValueConst* argv) {
    auto* host = host_of(context);
    std::string url;
    if (host != nullptr && argc > 0 && arg_string(context, argv[0], url)) {
        host->request_navigation(url);
    }
    return JS_UNDEFINED;
}

JSValue bp_submit_form(JSContext* context, JSValueConst, int argc,
                       JSValueConst* argv) {
    auto* host = host_of(context);
    std::string url;
    std::string method;
    std::string body;
    if (host != nullptr && argc > 0 && arg_string(context, argv[0], url)) {
        if (argc > 1) arg_string(context, argv[1], method);
        if (argc > 2) arg_string(context, argv[2], body);
        host->request_form_submission(url, method.empty() ? "GET" : method, body);
    }
    return JS_UNDEFINED;
}

JSValue bp_get_cookie(JSContext* context, JSValueConst, int, JSValueConst*) {
    auto* host = host_of(context);
    return JS_NewString(context, host == nullptr ? "" : host->cookie_header().c_str());
}

JSValue bp_set_cookie(JSContext* context, JSValueConst, int argc,
                      JSValueConst* argv) {
    auto* host = host_of(context);
    std::string cookie;
    if (host != nullptr && argc > 0 && arg_string(context, argv[0], cookie)) {
        host->set_cookie_string(cookie);
    }
    return JS_UNDEFINED;
}

JSValue bp_storage_keys(JSContext* context, JSValueConst, int argc,
                        JSValueConst* argv) {
    auto* host = host_of(context);
    std::string area;
    JSValue array = JS_NewArray(context);
    if (host == nullptr || argc < 1 || !arg_string(context, argv[0], area)) {
        return array;
    }
    const auto keys = host->storage_keys(area);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        JS_SetPropertyUint32(context, array, static_cast<std::uint32_t>(i),
                             JS_NewString(context, keys[i].c_str()));
    }
    return array;
}

JSValue bp_storage_get(JSContext* context, JSValueConst, int argc,
                       JSValueConst* argv) {
    auto* host = host_of(context);
    std::string area;
    std::string key;
    if (host == nullptr || argc < 2 || !arg_string(context, argv[0], area) ||
        !arg_string(context, argv[1], key)) {
        return JS_UNDEFINED;
    }
    const auto keys = host->storage_keys(area);
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        return JS_UNDEFINED;  // absent keys read as null in the DOM storage API
    }
    const auto value = host->storage_item(area, key);
    return JS_NewStringLen(context, value.data(), value.size());
}

JSValue bp_storage_set(JSContext* context, JSValueConst, int argc,
                       JSValueConst* argv) {
    auto* host = host_of(context);
    std::string area;
    std::string key;
    std::string value;
    if (host != nullptr && argc >= 2 && arg_string(context, argv[0], area) &&
        arg_string(context, argv[1], key)) {
        if (argc > 2) arg_string(context, argv[2], value);
        host->set_storage_item(area, key, value);
    }
    return JS_UNDEFINED;
}

JSValue bp_storage_remove(JSContext* context, JSValueConst, int argc,
                          JSValueConst* argv) {
    auto* host = host_of(context);
    std::string area;
    std::string key;
    if (host != nullptr && argc >= 2 && arg_string(context, argv[0], area) &&
        arg_string(context, argv[1], key)) {
        host->remove_storage_item(area, key);
    }
    return JS_UNDEFINED;
}

JSValue bp_storage_clear(JSContext* context, JSValueConst, int argc,
                         JSValueConst* argv) {
    auto* host = host_of(context);
    std::string area;
    if (host != nullptr && argc > 0 && arg_string(context, argv[0], area)) {
        host->clear_storage(area);
    }
    return JS_UNDEFINED;
}

JSValue bp_root(JSContext* context, JSValueConst, int, JSValueConst*) {
    auto* host = host_of(context);
    if (host == nullptr) return JS_NewInt64(context, 0);
    return JS_NewInt64(context, static_cast<std::int64_t>(host->root_element()));
}

JSValue bp_query_all(JSContext* context, JSValueConst, int argc,
                     JSValueConst* argv) {
    auto* host = host_of(context);
    std::string selector;
    if (host == nullptr || argc < 1 || !arg_string(context, argv[0], selector)) {
        return JS_NewArray(context);
    }
    return handles_to_array(context, host->query_selector_all(selector));
}

JSValue bp_query_scope(JSContext* context, JSValueConst, int argc,
                       JSValueConst* argv) {
    auto* host = host_of(context);
    std::string selector;
    if (host == nullptr || argc < 2 || JS_IsUndefined(argv[0]) ||
        !arg_string(context, argv[1], selector)) {
        return JS_NewArray(context);
    }
    return handles_to_array(
        context, host->query_selector_all_in(arg_handle(context, argv[0]), selector));
}

JSValue bp_matches(JSContext* context, JSValueConst, int argc,
                   JSValueConst* argv) {
    auto* host = host_of(context);
    std::string selector;
    if (host == nullptr || argc < 2 || !arg_string(context, argv[1], selector)) {
        return JS_FALSE;
    }
    return JS_NewBool(context,
                      host->element_matches(arg_handle(context, argv[0]), selector));
}

JSValue bp_node_type(JSContext* context, JSValueConst, int argc,
                     JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewInt32(context, 0);
    return JS_NewInt32(context,
                       host->element_node_type(arg_handle(context, argv[0])));
}

JSValue bp_tag_name(JSContext* context, JSValueConst, int argc,
                    JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewString(context, "");
    const auto name = host->element_tag_name(arg_handle(context, argv[0]));
    return JS_NewStringLen(context, name.data(), name.size());
}

JSValue bp_attr(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* host = host_of(context);
    std::string name;
    if (host == nullptr || argc < 2 || !arg_string(context, argv[1], name)) {
        return JS_UNDEFINED;
    }
    const auto handle = arg_handle(context, argv[0]);
    if (!host->element_has_attribute(handle, name)) return JS_UNDEFINED;
    const auto value = host->element_attribute(handle, name);
    return JS_NewStringLen(context, value.data(), value.size());
}

JSValue bp_has_attr(JSContext* context, JSValueConst, int argc,
                    JSValueConst* argv) {
    auto* host = host_of(context);
    std::string name;
    if (host == nullptr || argc < 2 || !arg_string(context, argv[1], name)) {
        return JS_FALSE;
    }
    return JS_NewBool(context,
                      host->element_has_attribute(arg_handle(context, argv[0]), name));
}

JSValue bp_set_attr(JSContext* context, JSValueConst, int argc,
                    JSValueConst* argv) {
    auto* host = host_of(context);
    std::string name;
    std::string value;
    if (host != nullptr && argc >= 2 && arg_string(context, argv[1], name)) {
        if (argc > 2) arg_string(context, argv[2], value);
        host->element_set_attribute(arg_handle(context, argv[0]), name, value);
    }
    return JS_UNDEFINED;
}

JSValue bp_del_attr(JSContext* context, JSValueConst, int argc,
                    JSValueConst* argv) {
    auto* host = host_of(context);
    std::string name;
    if (host != nullptr && argc >= 2 && arg_string(context, argv[1], name)) {
        host->element_remove_attribute(arg_handle(context, argv[0]), name);
    }
    return JS_UNDEFINED;
}

JSValue bp_attrs(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewArray(context);
    return pairs_to_array(context,
                          host->element_attributes(arg_handle(context, argv[0])));
}

JSValue bp_text(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewString(context, "");
    const auto text = host->element_text(arg_handle(context, argv[0]));
    return JS_NewStringLen(context, text.data(), text.size());
}

JSValue bp_set_text(JSContext* context, JSValueConst, int argc,
                    JSValueConst* argv) {
    auto* host = host_of(context);
    std::string text;
    if (host != nullptr && argc >= 1) {
        arg_string(context, argv[1], text);
        host->element_set_text(arg_handle(context, argv[0]), text);
    }
    return JS_UNDEFINED;
}

JSValue bp_inner_html(JSContext* context, JSValueConst, int argc,
                      JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewString(context, "");
    const auto html = host->element_inner_html(arg_handle(context, argv[0]));
    return JS_NewStringLen(context, html.data(), html.size());
}

JSValue bp_set_inner_html(JSContext* context, JSValueConst, int argc,
                          JSValueConst* argv) {
    auto* host = host_of(context);
    std::string markup;
    if (host != nullptr && argc >= 2 && arg_string(context, argv[1], markup)) {
        host->element_set_inner_html(arg_handle(context, argv[0]), markup);
    }
    return JS_UNDEFINED;
}

JSValue bp_outer_html(JSContext* context, JSValueConst, int argc,
                      JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewString(context, "");
    const auto html = host->element_outer_html(arg_handle(context, argv[0]));
    return JS_NewStringLen(context, html.data(), html.size());
}

JSValue bp_child_nodes(JSContext* context, JSValueConst, int argc,
                       JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewArray(context);
    return handles_to_array(context,
                            host->element_child_nodes(arg_handle(context, argv[0])));
}

JSValue bp_parent_node(JSContext* context, JSValueConst, int argc,
                       JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewInt64(context, 0);
    return JS_NewInt64(context,
                       static_cast<std::int64_t>(host->element_parent(
                           arg_handle(context, argv[0]))));
}

JSValue bp_next_sibling(JSContext* context, JSValueConst, int argc,
                        JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewInt64(context, 0);
    return JS_NewInt64(context,
                       static_cast<std::int64_t>(host->element_next_sibling(
                           arg_handle(context, argv[0]))));
}

JSValue bp_prev_sibling(JSContext* context, JSValueConst, int argc,
                        JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_NewInt64(context, 0);
    return JS_NewInt64(context,
                       static_cast<std::int64_t>(host->element_previous_sibling(
                           arg_handle(context, argv[0]))));
}

JSValue bp_create_el(JSContext* context, JSValueConst, int argc,
                     JSValueConst* argv) {
    auto* host = host_of(context);
    std::string tag;
    if (host == nullptr || argc < 1 || !arg_string(context, argv[0], tag)) {
        return JS_NewInt64(context, 0);
    }
    return JS_NewInt64(context, static_cast<std::int64_t>(host->create_element(tag)));
}

JSValue bp_create_text(JSContext* context, JSValueConst, int argc,
                       JSValueConst* argv) {
    auto* host = host_of(context);
    std::string text;
    if (host == nullptr || argc < 1) return JS_NewInt64(context, 0);
    arg_string(context, argv[0], text);
    return JS_NewInt64(context, static_cast<std::int64_t>(host->create_text_node(text)));
}

JSValue bp_create_comment(JSContext* context, JSValueConst, int argc,
                          JSValueConst* argv) {
    auto* host = host_of(context);
    std::string text;
    if (host == nullptr || argc < 1) return JS_NewInt64(context, 0);
    arg_string(context, argv[0], text);
    return JS_NewInt64(context, static_cast<std::int64_t>(host->create_comment(text)));
}

JSValue bp_append_child(JSContext* context, JSValueConst, int argc,
                        JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 2) return JS_FALSE;
    return JS_NewBool(context,
                      host->append_child(arg_handle(context, argv[0]),
                                         arg_handle(context, argv[1])));
}

JSValue bp_insert_before(JSContext* context, JSValueConst, int argc,
                         JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 2) return JS_FALSE;
    ElementHandle reference = kNoElement;
    if (argc > 2 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
        reference = arg_handle(context, argv[2]);
    }
    return JS_NewBool(context,
                      host->insert_before(arg_handle(context, argv[0]),
                                          arg_handle(context, argv[1]), reference));
}

JSValue bp_detach(JSContext* context, JSValueConst, int argc,
                  JSValueConst* argv) {
    auto* host = host_of(context);
    if (host == nullptr || argc < 1) return JS_FALSE;
    return JS_NewBool(context, host->detach_element(arg_handle(context, argv[0])));
}

JSValue bp_parse_fragment(JSContext* context, JSValueConst, int argc,
                          JSValueConst* argv) {
    auto* host = host_of(context);
    std::string markup;
    if (host == nullptr || argc < 1 || !arg_string(context, argv[0], markup)) {
        return JS_NewArray(context);
    }
    return handles_to_array(context, host->parse_html_fragment(markup));
}

JSValue bp_resolve_url(JSContext* context, JSValueConst, int argc,
                       JSValueConst* argv) {
    std::string href;
    std::string base;
    JSValue object = JS_NewObject(context);
    if (argc < 1 || !arg_string(context, argv[0], href)) {
        JS_SetPropertyStr(context, object, "href", JS_NewString(context, ""));
        return object;
    }
    if (argc > 1) arg_string(context, argv[1], base);
    Url resolved;
    try {
        if (base.empty()) {
            resolved = parse_url(href);
        } else {
            resolved = parse_url(resolve_url(base, href));
        }
    } catch (const Error&) {
        JS_SetPropertyStr(context, object, "href", JS_NewString(context, ""));
        return object;
    }
    const std::string port = resolved.port;
    const bool default_port =
        (resolved.scheme == "https" && (port.empty() || port == "443")) ||
        (resolved.scheme == "http" && (port.empty() || port == "80"));
    const std::string host_port =
        resolved.host + (default_port || port.empty() ? "" : ":" + port);
    const std::string path =
        resolved.path.empty() ? (resolved.has_authority ? "/" : "") : resolved.path;
    std::string href_text = resolved.scheme.empty()
                                ? std::string{}
                                : resolved.scheme + "://" + host_port;
    href_text += path;
    if (resolved.has_query) href_text += "?" + resolved.query;
    if (resolved.has_fragment) href_text += "#" + resolved.fragment;
    JS_SetPropertyStr(context, object, "href",
                      JS_NewString(context, href_text.c_str()));
    JS_SetPropertyStr(context, object, "origin",
                      JS_NewString(context, resolved.origin().c_str()));
    JS_SetPropertyStr(context, object, "protocol",
                      JS_NewString(context, (resolved.scheme + ":").c_str()));
    JS_SetPropertyStr(context, object, "host",
                      JS_NewString(context, host_port.c_str()));
    JS_SetPropertyStr(context, object, "hostname",
                      JS_NewString(context, resolved.host.c_str()));
    JS_SetPropertyStr(context, object, "port",
                      JS_NewString(context, default_port ? "" : port.c_str()));
    JS_SetPropertyStr(context, object, "pathname",
                      JS_NewString(context, path.c_str()));
    JS_SetPropertyStr(context, object, "search",
                      JS_NewString(context,
                                  (resolved.has_query ? "?" + resolved.query : "")
                                      .c_str()));
    JS_SetPropertyStr(context, object, "hash",
                      JS_NewString(context,
                                  (resolved.has_fragment ? "#" + resolved.fragment : "")
                                      .c_str()));
    return object;
}

JSValue bp_base_url(JSContext* context, JSValueConst, int, JSValueConst*) {
    auto* host = host_of(context);
    if (host == nullptr) return JS_NewString(context, "");
    const auto url = host->base_url();
    return JS_NewStringLen(context, url.data(), url.size());
}

JSValue bp_print(JSContext* context, JSValueConst, int argc,
                 JSValueConst* argv);

JSValue bp_drain_jobs(JSContext* context, JSValueConst, int, JSValueConst*);

JSValue bp_to_bytes(JSContext* context, JSValueConst, int argc,
                    JSValueConst* argv) {
    std::string text;
    if (argc < 1 || !arg_string(context, argv[0], text)) {
        return JS_NewUint8ArrayCopy(context, nullptr, 0);
    }
    return JS_NewUint8ArrayCopy(
        context, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

const JSCFunctionListEntry kBindingFunctions[] = {
    JS_CFUNC_DEF("pageInfo", 0, bp_page_info),
    JS_CFUNC_DEF("setTitle", 1, bp_set_title),
    JS_CFUNC_DEF("navigatorInfo", 0, bp_navigator_info),
    JS_CFUNC_DEF("request", 4, bp_request),
    JS_CFUNC_DEF("navigate", 1, bp_navigate),
    JS_CFUNC_DEF("submitForm", 3, bp_submit_form),
    JS_CFUNC_DEF("getCookie", 0, bp_get_cookie),
    JS_CFUNC_DEF("setCookie", 1, bp_set_cookie),
    JS_CFUNC_DEF("storageKeys", 1, bp_storage_keys),
    JS_CFUNC_DEF("storageGet", 2, bp_storage_get),
    JS_CFUNC_DEF("storageSet", 3, bp_storage_set),
    JS_CFUNC_DEF("storageRemove", 2, bp_storage_remove),
    JS_CFUNC_DEF("storageClear", 1, bp_storage_clear),
    JS_CFUNC_DEF("root", 0, bp_root),
    JS_CFUNC_DEF("queryAll", 1, bp_query_all),
    JS_CFUNC_DEF("queryScope", 2, bp_query_scope),
    JS_CFUNC_DEF("matches", 2, bp_matches),
    JS_CFUNC_DEF("nodeType", 1, bp_node_type),
    JS_CFUNC_DEF("tagName", 1, bp_tag_name),
    JS_CFUNC_DEF("attr", 2, bp_attr),
    JS_CFUNC_DEF("hasAttr", 2, bp_has_attr),
    JS_CFUNC_DEF("setAttr", 3, bp_set_attr),
    JS_CFUNC_DEF("delAttr", 2, bp_del_attr),
    JS_CFUNC_DEF("attrs", 1, bp_attrs),
    JS_CFUNC_DEF("text", 1, bp_text),
    JS_CFUNC_DEF("setText", 2, bp_set_text),
    JS_CFUNC_DEF("innerHTML", 1, bp_inner_html),
    JS_CFUNC_DEF("setInnerHTML", 2, bp_set_inner_html),
    JS_CFUNC_DEF("outerHTML", 1, bp_outer_html),
    JS_CFUNC_DEF("childNodes", 1, bp_child_nodes),
    JS_CFUNC_DEF("parentNode", 1, bp_parent_node),
    JS_CFUNC_DEF("nextSibling", 1, bp_next_sibling),
    JS_CFUNC_DEF("prevSibling", 1, bp_prev_sibling),
    JS_CFUNC_DEF("createEl", 1, bp_create_el),
    JS_CFUNC_DEF("createText", 1, bp_create_text),
    JS_CFUNC_DEF("createComment", 1, bp_create_comment),
    JS_CFUNC_DEF("appendChild", 2, bp_append_child),
    JS_CFUNC_DEF("insertBefore", 3, bp_insert_before),
    JS_CFUNC_DEF("detach", 1, bp_detach),
    JS_CFUNC_DEF("parseFragment", 1, bp_parse_fragment),
    JS_CFUNC_DEF("resolveUrl", 2, bp_resolve_url),
    JS_CFUNC_DEF("baseUrl", 0, bp_base_url),
    JS_CFUNC_DEF("print", 2, bp_print),
    JS_CFUNC_DEF("drainJobs", 0, bp_drain_jobs),
    JS_CFUNC_DEF("toBytes", 1, bp_to_bytes),
};

// ---- The runtime ------------------------------------------------------------

class ExecutionScope {
public:
    ExecutionScope(QuickJavaScriptRuntime* owner, JSRuntime* runtime,
                   bool& active, const ScriptOptions& options);
    ~ExecutionScope();

    bool expired() { return interrupt_at_deadline(runtime_, &deadline_) != 0; }
    void add_time(std::chrono::milliseconds elapsed) {
        deadline_.expires_at += elapsed;
    }
    Deadline& deadline() { return deadline_; }

private:
    QuickJavaScriptRuntime* owner_;
    JSRuntime* runtime_;
    bool& active_;
    Deadline deadline_;
};

class QuickJavaScriptRuntime final : public JavaScriptRuntime {
public:
    explicit QuickJavaScriptRuntime(DocumentScriptHost* host = nullptr)
        : runtime_(JS_NewRuntime()),
          context_(runtime_ != nullptr ? JS_NewContext(runtime_.get()) : nullptr),
          document_host_(host) {
        if (runtime_ == nullptr || context_ == nullptr) throw std::bad_alloc();
        JS_SetCanBlock(runtime_.get(), false);
        JS_SetHostPromiseRejectionTracker(runtime_.get(),
                                          promise_rejection_tracker, nullptr);
        JS_SetContextOpaque(context_.get(), this);
        install_platform();
    }

    ~QuickJavaScriptRuntime() override {
        if (context_ != nullptr) {
            JS_SetContextOpaque(context_.get(), nullptr);
        }
    }

    void forward_console(ConsoleMessage message) {
        if (console_handler_) {
            try {
                auto handler = console_handler_;
                handler(message);
            } catch (...) {
                // A console handler must not break page evaluation.
            }
        }
    }

    int run_pending_jobs(int max_jobs) {
        int executed = 0;
        while (executed < max_jobs && JS_IsJobPending(runtime_.get())) {
            JSContext* job_context = nullptr;
            const int status = JS_ExecutePendingJob(runtime_.get(), &job_context);
            ++executed;
            if (status < 0 && job_context != nullptr) {
                (void)take_exception(job_context);
            }
        }
        return executed;
    }

    void extend_deadline(std::chrono::milliseconds elapsed) {
        if (scope_ != nullptr && elapsed > std::chrono::milliseconds::zero()) {
            scope_->add_time(elapsed);
        }
    }

    ScriptResult evaluate(std::string_view script,
                          const ScriptOptions& options) override {
        if (active_) return {false, {}, "JavaScript runtime is already executing"};
        if (const auto error = validate_options(options); !error.empty()) {
            return {false, {}, error};
        }
        const std::string source(script);
        ExecutionScope scope(this, runtime_.get(), active_, options);
        JSValue value = JS_Eval(context_.get(), source.c_str(), source.size(),
                                "<prowsetk>", JS_EVAL_TYPE_GLOBAL);
        ScriptResult result;
        if (JS_IsException(value)) {
            result.error = take_exception(context_.get());
        } else {
            result.value = value_to_string(context_.get(), value);
            if (JS_HasException(context_.get())) {
                result.value.clear();
                result.error = take_exception(context_.get());
            } else {
                result.ok = true;
            }
        }
        JS_FreeValue(context_.get(), value);
        if (scope.expired()) return {false, {}, "JavaScript execution deadline exceeded"};
        const auto checkpoint = drain_microtasks(options, scope);
        if (!checkpoint.ok && result.ok) return checkpoint;
        return result;
    }

    ScriptResult run_microtasks(const ScriptOptions& options) override {
        if (active_) return {false, {}, "JavaScript runtime is already executing"};
        if (const auto error = validate_options(options); !error.empty()) {
            return {false, {}, error};
        }
        ExecutionScope scope(this, runtime_.get(), active_, options);
        return drain_microtasks(options, scope);
    }

    bool has_pending_microtasks() const override {
        return JS_IsJobPending(runtime_.get());
    }

    void set_global(std::string_view name, std::string_view value) override {
        if (active_) return;
        ExecutionScope scope(this, runtime_.get(), active_, ScriptOptions{});
        JSValue global = JS_GetGlobalObject(context_.get());
        const JSAtom property = JS_NewAtomLen(context_.get(), name.data(), name.size());
        if (property != JS_ATOM_NULL) {
            JSValue string_value = JS_NewStringLen(context_.get(), value.data(), value.size());
            if (!JS_IsException(string_value)) {
                JS_SetProperty(context_.get(), global, property, string_value);
            }
            JS_FreeAtom(context_.get(), property);
        }
        if (JS_HasException(context_.get())) (void)take_exception(context_.get());
        JS_FreeValue(context_.get(), global);
    }

    void set_console_handler(ConsoleHandler handler) override {
        console_handler_ = std::move(handler);
    }

    void set_document_host(DocumentScriptHost* host) override {
        document_host_ = host;
    }

    DocumentScriptHost* document_host() const noexcept {
        return document_host_;
    }

    std::string name() const override { return "quickjs"; }

    CapabilitySet capabilities() const override {
        CapabilitySet capabilities;
        capabilities.set("javascript", ImplementationClass::FullyImplemented,
                         "QuickJS page scripting runtime");
        capabilities.set("console", ImplementationClass::PartiallyImplemented,
                         "log/info/warn/error/debug; space-joined string conversion, no format substitutions");
        capabilities.set("promises", ImplementationClass::ImplementedWithRestrictions,
                         "ECMAScript promises; bounded checkpoints after evaluation, no implicit promise unwrapping or rejection events");
        capabilities.set("queueMicrotask", ImplementationClass::ImplementedWithRestrictions,
                         "FIFO jobs; bounded checkpoints, remaining jobs retained on limit or callback failure");
        capabilities.set("dom", ImplementationClass::PartiallyImplemented,
                         "handle-based Flatworm DOM bridge: querySelector(All), createElement/TextNode/Comment, attributes, textContent/innerHTML/outerHTML, tree mutation, classList, dataset, style, form submit");
        capabilities.set("eventtarget", ImplementationClass::PartiallyImplemented,
                         "window/document/element listeners and synthetic events; click and submit drive navigation, no capture-phase or DOM event objects from real input");
        capabilities.set("xmlhttprequest", ImplementationClass::ImplementedWithRestrictions,
                         "sync and async XHR over the host-mediated NetworkClient; no progress events, upload streams, timeouts, or CORS enforcement");
        capabilities.set("fetch", ImplementationClass::ImplementedWithRestrictions,
                         "fetch with Headers/Response over the host-mediated NetworkClient; responses resolve through microtasks, no streaming bodies");
        capabilities.set("timers", ImplementationClass::PartiallyImplemented,
                         "setTimeout/setInterval/requestAnimationFrame drained by bounded flush passes after each document's scripts");
        capabilities.set("storage", ImplementationClass::PartiallyImplemented,
                         "localStorage/sessionStorage and document.cookie backed by the session storage and cookie jar");
        capabilities.set("url", ImplementationClass::PartiallyImplemented,
                         "URL/URLSearchParams polyfills over the engine URL parser");
        capabilities.set("text-encoding", ImplementationClass::PartiallyImplemented,
                         "TextEncoder/TextDecoder in the web platform shim; UTF-8 and "
                         "windows-1252 labels, encodeInto, fatal and streaming modes");
        capabilities.set("location", ImplementationClass::PartiallyImplemented,
                         "location reads resolve against the live document; assignment and form submit trigger a host navigation after the script pass");
        capabilities.set("navigator", ImplementationClass::PartiallyImplemented,
                         "static navigator fields from the session configuration");
        return capabilities;
    }

private:
    ScriptResult drain_microtasks(const ScriptOptions& options, ExecutionScope& scope) {
        std::size_t executed = 0;
        while (JS_IsJobPending(runtime_.get())) {
            if (scope.expired()) return {false, {}, "JavaScript execution deadline exceeded"};
            if (executed == options.max_microtask_jobs) {
                return {false, {}, "JavaScript microtask job limit exceeded"};
            }
            JSContext* job_context = nullptr;
            const int status = JS_ExecutePendingJob(runtime_.get(), &job_context);
            ++executed;
            if (status < 0) {
                return {false, {}, take_exception(job_context != nullptr ? job_context : context_.get())};
            }
        }
        if (scope.expired()) return {false, {}, "JavaScript execution deadline exceeded"};
        return {true, "undefined", {}};
    }

    void install_platform() {
        JSContext* context = context_.get();
        if (install_console_binding(context) < 0) throw std::bad_alloc();
        JSValue global = JS_GetGlobalObject(context);
        if (JS_SetPropertyStr(
                context, global, "queueMicrotask",
                JS_NewCFunction(context, queue_microtask, "queueMicrotask", 1)) < 0) {
            JS_FreeValue(context, global);
            throw std::bad_alloc();
        }
        JSValue bindings = JS_NewObject(context);
        if (JS_SetPropertyFunctionList(context, bindings, kBindingFunctions,
                                       sizeof(kBindingFunctions) /
                                           sizeof(kBindingFunctions[0])) < 0) {
            JS_FreeValue(context, bindings);
            JS_FreeValue(context, global);
            throw std::bad_alloc();
        }
        JS_SetPropertyStr(context, global, "__prowsetk", bindings);
        JS_Eval(context, kWebPlatformShim, sizeof(kWebPlatformShim) - 1,
                "<prowsetk-web-platform>", JS_EVAL_TYPE_GLOBAL);
        if (JS_HasException(context)) {
            JS_FreeValue(context, global);
            throw std::runtime_error("web platform shim failed to load: " +
                                     take_exception(context));
        }
        // window/self alias the global object, matching browser semantics.
        JS_SetPropertyStr(context, global, "window", JS_DupValue(context, global));
        JS_SetPropertyStr(context, global, "self", JS_DupValue(context, global));
        JS_SetPropertyStr(context, global, "top", JS_DupValue(context, global));
        JS_SetPropertyStr(context, global, "parent", JS_DupValue(context, global));
        JS_SetPropertyStr(context, global, "frames", JS_DupValue(context, global));
        JS_SetPropertyStr(context, global, "globalThis", JS_DupValue(context, global));
        JS_FreeValue(context, global);
    }

    RuntimePtr runtime_;
    ContextPtr context_;
    ConsoleHandler console_handler_;
    DocumentScriptHost* document_host_ = nullptr;
    bool active_ = false;
    ExecutionScope* scope_ = nullptr;

    friend class ExecutionScope;
};

ExecutionScope::ExecutionScope(QuickJavaScriptRuntime* owner, JSRuntime* runtime,
                               bool& active, const ScriptOptions& options)
    : owner_(owner),
      runtime_(runtime),
      active_(active),
      deadline_{std::chrono::steady_clock::now() +
                std::chrono::milliseconds(options.timeout_ms)} {
    active_ = true;
    owner_->scope_ = this;
    JS_UpdateStackTop(runtime_);
    JS_SetMemoryLimit(runtime_, options.memory_limit_bytes);
    JS_SetInterruptHandler(runtime_, interrupt_at_deadline, &deadline_);
}

ExecutionScope::~ExecutionScope() {
    JS_SetInterruptHandler(runtime_, nullptr, nullptr);
    JS_SetMemoryLimit(runtime_, std::numeric_limits<std::size_t>::max());
    owner_->scope_ = nullptr;
    active_ = false;
}

QuickJavaScriptRuntime* runtime_of(JSContext* context) {
    return static_cast<QuickJavaScriptRuntime*>(JS_GetContextOpaque(context));
}

DocumentScriptHost* host_of(JSContext* context) {
    auto* runtime = runtime_of(context);
    return runtime == nullptr ? nullptr : runtime->document_host();
}

// ---- Runtime-dependent bindings (need the complete runtime type) ----------

JSValue bp_request(JSContext* context, JSValueConst, int argc,
                   JSValueConst* argv) {
    auto* host = host_of(context);
    HostRequest request;
    if (argc > 0) arg_string(context, argv[0], request.method);
    if (argc > 1) arg_string(context, argv[1], request.url);
    if (argc > 2) request.headers = arg_header_pairs(context, argv[2]);
    if (argc > 3) arg_string(context, argv[3], request.body);

    HostResponse response;
    if (host == nullptr) {
        response.error = "no document host is attached";
    } else {
        auto* runtime = runtime_of(context);
        const auto started = std::chrono::steady_clock::now();
        response = host->host_request(request);
        if (runtime != nullptr) {
            // Host-mediated network time must not consume the script budget.
            runtime->extend_deadline(std::chrono::duration_cast<
                                     std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started));
        }
    }

    JSValue object = JS_NewObject(context);
    JS_SetPropertyStr(context, object, "ok", JS_NewBool(context, response.ok));
    JS_SetPropertyStr(context, object, "error",
                      JS_NewString(context, response.error.c_str()));
    JS_SetPropertyStr(context, object, "status",
                      JS_NewInt64(context, response.status));
    JS_SetPropertyStr(context, object, "statusText",
                      JS_NewString(context, response.status_text.c_str()));
    JS_SetPropertyStr(context, object, "finalUrl",
                      JS_NewString(context, response.final_url.c_str()));
    JS_SetPropertyStr(context, object, "headers",
                      pairs_to_array(context, response.headers));
    JS_SetPropertyStr(context, object, "body",
                      JS_NewString(context, response.body.c_str()));
    return object;
}

JSValue bp_print(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    auto* runtime = runtime_of(context);
    std::string level;
    std::string text;
    if (runtime != nullptr && argc >= 2 && arg_string(context, argv[0], level) &&
        arg_string(context, argv[1], text)) {
        runtime->forward_console(ConsoleMessage{std::move(level), std::move(text)});
    }
    return JS_UNDEFINED;
}

JSValue bp_drain_jobs(JSContext* context, JSValueConst, int, JSValueConst*) {
    auto* runtime = runtime_of(context);
    if (runtime == nullptr) return JS_NewInt32(context, 0);
    return JS_NewInt32(context, runtime->run_pending_jobs(100));
}

JSValue microtask_job(JSContext* context, int, JSValueConst* argv) {
    return JS_Call(context, argv[0], JS_UNDEFINED, 0, nullptr);
}

JSValue queue_microtask(JSContext* context, JSValueConst, int argc,
                        JSValueConst* argv) {
    if (argc == 0 || !JS_IsFunction(context, argv[0])) {
        return JS_ThrowTypeError(context, "queueMicrotask requires a callable");
    }
    if (JS_EnqueueJob(context, microtask_job, 1, argv) < 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

// QuickJS host promise rejection hook. Without it, a rejected promise with no
// handler vanishes silently; browsers report those to the console, and page
// applications routinely swallow their bootstrap failures exactly this way.
// Forward the rejection through the console channel as an "error" message so
// drivers and tests can observe it.
void promise_rejection_tracker(JSContext* context, JSValueConst,
                               JSValueConst reason, bool is_handled,
                               void*) {
    if (is_handled || context == nullptr) return;
    auto* runtime = runtime_of(context);
    if (runtime == nullptr) return;
    std::string text = "Uncaught (in promise) ";
    text += value_to_string(context, reason);
    if (JS_IsError(reason)) {
        JSValue stack = JS_GetPropertyStr(context, reason, "stack");
        if (!JS_IsUndefined(stack)) {
            const std::string stack_text = value_to_string(context, stack);
            if (!stack_text.empty()) {
                text.push_back('\n');
                text += stack_text;
            }
        }
        JS_FreeValue(context, stack);
    }
    if (JS_HasException(context)) {
        JS_FreeValue(context, JS_GetException(context));
    }
    try {
        runtime->forward_console(ConsoleMessage{"error", std::move(text)});
    } catch (...) {
        // Reporting must never break promise resolution.
    }
}

// ---- Console ----------------------------------------------------------------

JSValue console_method(JSContext* context, const char* level, int argc,
                       JSValueConst* argv) {
    auto* runtime = runtime_of(context);
    if (runtime == nullptr) {
        return JS_UNDEFINED;
    }
    ConsoleMessage message;
    message.level = level;
    for (int i = 0; i < argc; ++i) {
        if (i != 0) message.text += ' ';
        message.text += value_to_string(context, argv[i]);
        if (JS_HasException(context)) return JS_EXCEPTION;
    }
    runtime->forward_console(std::move(message));
    return JS_UNDEFINED;
}

JSValue console_log(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    return console_method(context, "log", argc, argv);
}
JSValue console_info(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    return console_method(context, "info", argc, argv);
}
JSValue console_warn(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    return console_method(context, "warn", argc, argv);
}
JSValue console_error(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    return console_method(context, "error", argc, argv);
}
JSValue console_debug(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
    return console_method(context, "debug", argc, argv);
}

int install_console_binding(JSContext* context) {
    JSValue global = JS_GetGlobalObject(context);
    JSValue console = JS_NewObject(context);
    JS_SetPropertyStr(context, console, "log",
                      JS_NewCFunction(context, console_log, "log", 1));
    JS_SetPropertyStr(context, console, "info",
                      JS_NewCFunction(context, console_info, "info", 1));
    JS_SetPropertyStr(context, console, "warn",
                      JS_NewCFunction(context, console_warn, "warn", 1));
    JS_SetPropertyStr(context, console, "error",
                      JS_NewCFunction(context, console_error, "error", 1));
    JS_SetPropertyStr(context, console, "debug",
                      JS_NewCFunction(context, console_debug, "debug", 1));
    const int rc = JS_SetPropertyStr(context, global, "console", console);
    // JS_SetPropertyStr transfers ownership of `console`; do not free it here.
    JS_FreeValue(context, global);
    return rc;
}

}  // namespace

std::unique_ptr<JavaScriptRuntime> make_javascript_runtime(DocumentScriptHost* host) {
    return std::make_unique<QuickJavaScriptRuntime>(host);
}

}  // namespace prowsetk

#else

namespace prowsetk {

std::unique_ptr<JavaScriptRuntime> make_javascript_runtime(DocumentScriptHost*) {
    return make_null_javascript_runtime();
}

}  // namespace prowsetk

#endif
