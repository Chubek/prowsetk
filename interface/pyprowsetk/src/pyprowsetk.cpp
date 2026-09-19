#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/map.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/filesystem.h>

#include <prowsetk/browser.hpp>
#include <prowsetk/document.hpp>
#include <prowsetk/network_client.hpp>
#include <prowsetk/storage.hpp>
#include <prowsetk/event.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/capability.hpp>
#include <prowsetk/web_platform.hpp>
#include <prowsetk/javascript_runtime.hpp>
#include <prowsetk/lua_runtime.hpp>
#include <prowsetk/endpoint_extraction.hpp>
#include <prowsetk/ir.hpp>
#include <prowsetk/xpath.hpp>
#include <prowsetk/url.hpp>
#include <prowsetk/redaction.hpp>
#include <prowsetk/version.hpp>
#include <prowsetk/project_config.hpp>
#include <prowsetk/plugin_registry.hpp>
#include <prowsetk/wasm_runtime.hpp>
#include <prowsetk/web_interface.hpp>
#include <prowsetk/error.hpp>

namespace nb = nanobind;
using namespace prowsetk;

NB_MODULE(_core, m) {
    m.doc() = "ProwseTk Python bindings (nanobind) - full engine access";

    // ---- Error handling ----
    nb::enum_<ErrorCode>(m, "ErrorCode")
        .value("Ok", ErrorCode::Ok)
        .value("InvalidArgument", ErrorCode::InvalidArgument)
        .value("InvalidUrl", ErrorCode::InvalidUrl)
        .value("ParseError", ErrorCode::ParseError)
        .value("NotFound", ErrorCode::NotFound)
        .value("Unsupported", ErrorCode::Unsupported)
        .value("NetworkError", ErrorCode::NetworkError)
        .value("Timeout", ErrorCode::Timeout)
        .value("TooManyRedirects", ErrorCode::TooManyRedirects)
        .value("ResourceLimit", ErrorCode::ResourceLimit)
        .value("SecurityViolation", ErrorCode::SecurityViolation)
        .value("PluginError", ErrorCode::PluginError)
        .value("WasmError", ErrorCode::WasmError)
        .value("LuaError", ErrorCode::LuaError)
        .value("JavaScriptError", ErrorCode::JavaScriptError)
        .value("StorageError", ErrorCode::StorageError)
        .value("IoError", ErrorCode::IoError)
        .value("Internal", ErrorCode::Internal)
        .export_values();

    nb::exception<Error> exc(m, "ProwseTkError");
    // translator: prowsetk::Error -> ProwseTkError
    nb::register_exception_translator([](const std::exception_ptr &p, void * /*payload*/) {
        try {
            std::rethrow_exception(p);
        } catch (const Error &e) {
            PyErr_SetString(PyExc_RuntimeError, e.what());
            // We set a Python exception with message including code
            // Also expose as ProwseTkError if available
            // nanobind will map to RuntimeError fallback; try to set ProwseTkError
            // by looking up the type
            // For simplicity, raise RuntimeError with code prefix
        }
    });

    m.def("version", &version, "Return ProwseTk version string");
    m.attr("VERSION") = std::string(PROWSETK_VERSION_STRING);
    m.attr("VERSION_MAJOR") = PROWSETK_VERSION_MAJOR;
    m.attr("VERSION_MINOR") = PROWSETK_VERSION_MINOR;
    m.attr("VERSION_PATCH") = PROWSETK_VERSION_PATCH;

    // ---- Url ----
    nb::class_<Url>(m, "Url")
        .def(nb::init<>())
        .def_rw("scheme", &Url::scheme)
        .def_rw("userinfo", &Url::userinfo)
        .def_rw("host", &Url::host)
        .def_rw("port", &Url::port)
        .def_rw("path", &Url::path)
        .def_rw("query", &Url::query)
        .def_rw("fragment", &Url::fragment)
        .def_rw("has_authority", &Url::has_authority)
        .def_rw("has_query", &Url::has_query)
        .def_rw("has_fragment", &Url::has_fragment)
        .def("is_absolute", &Url::is_absolute)
        .def("has_host", &Url::has_host)
        .def("origin", &Url::origin)
        .def("to_string", &Url::to_string)
        .def("__repr__", [](const Url &u){ return "<Url " + u.to_string() + ">"; });

    m.def("parse_url", &parse_url, nb::arg("input"));
    m.def("resolve_url", nb::overload_cast<std::string_view, std::string_view>(&resolve_url), nb::arg("base"), nb::arg("reference"));
    m.def("normalize_url", &normalize_url, nb::arg("input"));

    // ---- Capability ----
    nb::enum_<ImplementationClass>(m, "ImplementationClass")
        .value("FullyImplemented", ImplementationClass::FullyImplemented)
        .value("PartiallyImplemented", ImplementationClass::PartiallyImplemented)
        .value("ImplementedWithRestrictions", ImplementationClass::ImplementedWithRestrictions)
        .value("DummyImplementation", ImplementationClass::DummyImplementation)
        .value("Unsupported", ImplementationClass::Unsupported)
        .export_values();

    nb::class_<Capability>(m, "Capability")
        .def(nb::init<>())
        .def_rw("name", &Capability::name)
        .def_rw("classification", &Capability::classification)
        .def_rw("notes", &Capability::notes)
        .def("__repr__", [](const Capability &c){ return "<Capability " + c.name + " " + std::string(to_string(c.classification)) + ">"; });

    nb::class_<CapabilitySet>(m, "CapabilitySet")
        .def(nb::init<>())
        .def("add", &CapabilitySet::add)
        .def("set", &CapabilitySet::set, nb::arg("name"), nb::arg("classification"), nb::arg("notes") = "")
        .def("has", &CapabilitySet::has, nb::arg("name"))
        .def("find", [](const CapabilitySet &cs, std::string_view name) -> const Capability* { return cs.find(name); }, nb::rv_policy::reference_internal)
        .def("classification", &CapabilitySet::classification, nb::arg("name"))
        .def("all", &CapabilitySet::all);

    nb::class_<WebPlatform>(m, "WebPlatform")
        .def(nb::init<>())
        .def("declare", &WebPlatform::declare, nb::arg("name"), nb::arg("classification"), nb::arg("notes") = "")
        .def("supports", &WebPlatform::supports, nb::arg("api"))
        .def("classification", &WebPlatform::classification, nb::arg("api"))
        .def_prop_ro("capabilities", [](const WebPlatform &wp) { return wp.capabilities(); });
    m.def("default_web_platform", &default_web_platform);

    // ---- Redaction ----
    nb::class_<RedactionPolicy>(m, "RedactionPolicy")
        .def(nb::init<>())
        .def_rw("header_names", &RedactionPolicy::header_names)
        .def_rw("query_parameter_names", &RedactionPolicy::query_parameter_names)
        .def_rw("replacement", &RedactionPolicy::replacement)
        .def_static("defaults", &RedactionPolicy::defaults);

    nb::class_<Redactor>(m, "Redactor")
        .def(nb::init<>())
        .def(nb::init<RedactionPolicy>(), nb::arg("policy"))
        .def_prop_ro("policy", &Redactor::policy)
        .def("is_sensitive_header", &Redactor::is_sensitive_header)
        .def("is_sensitive_query_parameter", &Redactor::is_sensitive_query_parameter)
        .def("redact_header", &Redactor::redact_header)
        .def("redact_url", &Redactor::redact_url)
        .def("redact_headers", &Redactor::redact_headers);

    // ---- Event ----
    nb::enum_<EventType>(m, "EventType")
        .value("SessionCreated", EventType::SessionCreated)
        .value("SessionDestroyed", EventType::SessionDestroyed)
        .value("BeforeNavigation", EventType::BeforeNavigation)
        .value("AfterNavigation", EventType::AfterNavigation)
        .value("BeforeRequest", EventType::BeforeRequest)
        .value("AfterResponse", EventType::AfterResponse)
        .value("BeforeRedirect", EventType::BeforeRedirect)
        .value("DocumentCreated", EventType::DocumentCreated)
        .value("BeforeScript", EventType::BeforeScript)
        .value("AfterScript", EventType::AfterScript)
        .value("ScriptException", EventType::ScriptException)
        .value("Console", EventType::Console)
        .value("DomMutation", EventType::DomMutation)
        .value("CookieChange", EventType::CookieChange)
        .value("StorageAccess", EventType::StorageAccess)
        .value("UnsupportedApi", EventType::UnsupportedApi)
        .value("PluginInit", EventType::PluginInit)
        .value("PluginShutdown", EventType::PluginShutdown)
        .value("EndpointDiscovered", EventType::EndpointDiscovered)
        .export_values();
    m.def("event_type_to_string", static_cast<const char*(*)(EventType)>(&to_string), nb::arg("type"));
    m.def("parse_event_type", &parse_event_type, nb::arg("name"));

    nb::class_<Event>(m, "Event")
        .def(nb::init<>())
        .def_rw("type", &Event::type)
        .def_rw("url", &Event::url)
        .def_rw("name", &Event::name)
        .def_rw("message", &Event::message)
        .def_rw("attributes", &Event::attributes)
        .def_rw("cancelled", &Event::cancelled)
        .def_rw("session_id", &Event::session_id)
        .def("__repr__", [](const Event &e){ return "<Event " + std::string(to_string(e.type)) + " " + e.url + ">"; });

    nb::class_<EventDispatcher>(m, "EventDispatcher")
        .def(nb::init<>())
        .def("subscribe", &EventDispatcher::subscribe, nb::arg("type"), nb::arg("handler"))
        .def("subscribe_all", &EventDispatcher::subscribe_all, nb::arg("handler"))
        .def("unsubscribe", &EventDispatcher::unsubscribe, nb::arg("id"))
        .def("clear", &EventDispatcher::clear)
        .def("emit", [](EventDispatcher &d, Event &e){ d.emit(e); }, nb::arg("event"))
        .def("handler_count", &EventDispatcher::handler_count, nb::arg("type"));

    // ---- Network ----
    nb::class_<HttpRequest>(m, "HttpRequest")
        .def(nb::init<>())
        .def_rw("method", &HttpRequest::method)
        .def_rw("url", &HttpRequest::url)
        .def_rw("headers", &HttpRequest::headers)
        .def_rw("body", &HttpRequest::body)
        .def_rw("timeout_ms", &HttpRequest::timeout_ms)
        .def_rw("max_response_bytes", &HttpRequest::max_response_bytes)
        .def("__repr__", [](const HttpRequest &r){ return "<HttpRequest " + r.method + " " + r.url + ">"; });

    nb::class_<HttpResponse>(m, "HttpResponse")
        .def(nb::init<>())
        .def_rw("status", &HttpResponse::status)
        .def_rw("headers", &HttpResponse::headers)
        .def_rw("body", &HttpResponse::body)
        .def_rw("final_url", &HttpResponse::final_url)
        .def_rw("redirect_chain", &HttpResponse::redirect_chain)
        .def("ok", &HttpResponse::ok)
        .def("header", &HttpResponse::header, nb::arg("name"))
        .def("__repr__", [](const HttpResponse &r){ return "<HttpResponse " + std::to_string(r.status) + ">"; });

    nb::class_<NetworkClient>(m, "NetworkClient")
        .def("send", &NetworkClient::send, nb::arg("request"))
        .def("name", &NetworkClient::name);

    nb::class_<MemoryNetworkClient, NetworkClient>(m, "MemoryNetworkClient")
        .def(nb::init<>())
        .def("set_response", &MemoryNetworkClient::set_response, nb::arg("url"), nb::arg("response"))
        .def("set_handler", &MemoryNetworkClient::set_handler, nb::arg("handler"))
        .def("send", &MemoryNetworkClient::send, nb::arg("request"))
        .def("requests", &MemoryNetworkClient::requests, nb::rv_policy::reference_internal);

    m.def("make_socket_network_client", &make_socket_network_client);

    // ---- Storage ----
    nb::class_<Cookie>(m, "Cookie")
        .def(nb::init<>())
        .def_rw("name", &Cookie::name)
        .def_rw("value", &Cookie::value)
        .def_rw("domain", &Cookie::domain)
        .def_rw("path", &Cookie::path)
        .def_rw("secure", &Cookie::secure)
        .def_rw("http_only", &Cookie::http_only)
        .def_rw("host_only", &Cookie::host_only)
        .def_rw("expires_unix", &Cookie::expires_unix)
        .def_rw("same_site", &Cookie::same_site);

    nb::class_<CookieJar>(m, "CookieJar")
        .def("set", &CookieJar::set, nb::arg("origin"), nb::arg("cookie"))
        .def("get", &CookieJar::get, nb::arg("origin"))
        .def("cookie_header", &CookieJar::cookie_header, nb::arg("origin"))
        .def("clear", &CookieJar::clear)
        .def("all", &CookieJar::all)
        .def("set_access_listener", &CookieJar::set_access_listener, nb::arg("listener"));

    nb::class_<KeyValueStore>(m, "KeyValueStore")
        .def("get", &KeyValueStore::get, nb::arg("key"))
        .def("set", &KeyValueStore::set, nb::arg("key"), nb::arg("value"))
        .def("remove", &KeyValueStore::remove, nb::arg("key"))
        .def("clear", &KeyValueStore::clear)
        .def("keys", &KeyValueStore::keys)
        .def("set_access_listener", &KeyValueStore::set_access_listener, nb::arg("listener"))
        .def("__getitem__", [](KeyValueStore &s, std::string_view k){
            auto v = s.get(k);
            if (!v) throw nb::key_error(std::string(k).c_str());
            return *v;
        })
        .def("__setitem__", [](KeyValueStore &s, std::string k, std::string v){ s.set(std::move(k), std::move(v)); })
        .def("__contains__", [](KeyValueStore &s, std::string_view k){ return s.get(k).has_value(); })
        .def("__len__", [](KeyValueStore &s){ return s.keys().size(); });

    nb::class_<Storage>(m, "Storage")
        .def("cookies", &Storage::cookies, nb::rv_policy::reference_internal)
        .def("local_storage", &Storage::local_storage, nb::arg("session_id"), nb::rv_policy::reference_internal)
        .def("session_storage", &Storage::session_storage, nb::arg("session_id"), nb::rv_policy::reference_internal)
        .def("release_session", &Storage::release_session, nb::arg("session_id"))
        .def("set_access_listener", &Storage::set_access_listener, nb::arg("listener"));

    nb::class_<MemoryStorage, Storage>(m, "MemoryStorage")
        .def(nb::init<>());

    // ---- JavaScript ----
    nb::class_<ScriptOptions>(m, "ScriptOptions")
        .def(nb::init<>())
        .def_rw("timeout_ms", &ScriptOptions::timeout_ms)
        .def_rw("memory_limit_bytes", &ScriptOptions::memory_limit_bytes)
        .def_rw("max_microtask_jobs", &ScriptOptions::max_microtask_jobs);

    nb::class_<ScriptResult>(m, "ScriptResult")
        .def(nb::init<>())
        .def_rw("ok", &ScriptResult::ok)
        .def_rw("value", &ScriptResult::value)
        .def_rw("error", &ScriptResult::error);

    nb::class_<ConsoleMessage>(m, "ConsoleMessage")
        .def(nb::init<>())
        .def_rw("level", &ConsoleMessage::level)
        .def_rw("text", &ConsoleMessage::text);

    nb::class_<JavaScriptRuntime>(m, "JavaScriptRuntime")
        .def("evaluate", &JavaScriptRuntime::evaluate, nb::arg("script"), nb::arg("options") = ScriptOptions{})
        .def("run_microtasks", &JavaScriptRuntime::run_microtasks, nb::arg("options") = ScriptOptions{})
        .def("has_pending_microtasks", &JavaScriptRuntime::has_pending_microtasks)
        .def("set_global", &JavaScriptRuntime::set_global, nb::arg("name"), nb::arg("value"))
        .def("set_console_handler", &JavaScriptRuntime::set_console_handler, nb::arg("handler"))
        .def("name", &JavaScriptRuntime::name)
        .def("capabilities", &JavaScriptRuntime::capabilities);

    m.def("make_null_javascript_runtime", &make_null_javascript_runtime);
    m.def("make_javascript_runtime", &make_javascript_runtime);

    // ---- Lua ----
    nb::class_<LuaArgument>(m, "LuaArgument")
        .def(nb::init<>())
        .def_rw("name", &LuaArgument::name)
        .def_rw("type", &LuaArgument::type)
        .def_rw("value", &LuaArgument::value);

    nb::class_<LuaResult>(m, "LuaResult")
        .def(nb::init<>())
        .def_rw("ok", &LuaResult::ok)
        .def_rw("error", &LuaResult::error);

    nb::class_<LuaRuntime>(m, "LuaRuntime")
        .def(nb::init<>())
        .def_static("available", &LuaRuntime::available)
        .def("bind_browser", &LuaRuntime::bind_browser, nb::arg("browser"))
        .def("bound_browser", &LuaRuntime::bound_browser, nb::rv_policy::reference)
        .def("run", &LuaRuntime::run, nb::arg("code"), nb::arg("chunk_name") = "<string>")
        .def("run_file", &LuaRuntime::run_file, nb::arg("path"))
        .def("call", &LuaRuntime::call, nb::arg("function_name"))
        .def("call_function", [](LuaRuntime &rt, std::string_view fn, const std::vector<LuaArgument> &args){
            std::string ret;
            auto res = rt.call_function(fn, args, &ret);
            return std::pair<LuaResult, std::string>(res, ret);
        }, nb::arg("function_name"), nb::arg("arguments"))
        .def("last_error", &LuaRuntime::last_error);

    // ---- Attribute & Element/Document ----
    nb::class_<Attribute>(m, "Attribute")
        .def(nb::init<>())
        .def_rw("name", &Attribute::name)
        .def_rw("value", &Attribute::value)
        .def("__repr__", [](const Attribute &a){ return a.name + "=\"" + a.value + "\""; });

    nb::class_<Element>(m, "Element")
        .def(nb::init<>())
        .def("valid", &Element::valid)
        .def("tag_name", &Element::tag_name)
        .def("id", &Element::id)
        .def("class_name", &Element::class_name)
        .def("has_attribute", &Element::has_attribute, nb::arg("name"))
        .def("attribute", &Element::attribute, nb::arg("name"))
        .def("set_attribute", &Element::set_attribute, nb::arg("name"), nb::arg("value"))
        .def("remove_attribute", &Element::remove_attribute, nb::arg("name"))
        .def("attributes", &Element::attributes)
        .def("text", &Element::text)
        .def("inner_html", &Element::inner_html)
        .def("outer_html", &Element::outer_html)
        .def("parent", &Element::parent)
        .def("children", &Element::children)
        .def("first_child", &Element::first_child)
        .def("next_sibling", &Element::next_sibling)
        .def("previous_sibling", &Element::previous_sibling)
        .def("query_selector", &Element::query_selector, nb::arg("selector"))
        .def("query_selector_all", &Element::query_selector_all, nb::arg("selector"))
        .def("matches", &Element::matches, nb::arg("selector"))
        .def("value", &Element::value)
        .def("set_value", &Element::set_value, nb::arg("value"))
        .def("append_child", &Element::append_child, nb::arg("child"))
        .def("remove_child", &Element::remove_child, nb::arg("child"))
        .def("set_text", &Element::set_text, nb::arg("value"))
        .def("__bool__", &Element::valid)
        .def("__repr__", [](const Element &e){ return e.valid() ? "<Element " + e.tag_name() + ">" : "<Element invalid>"; })
        .def("__getitem__", [](const Element &e, std::string_view k){
            if (!e.has_attribute(k)) throw nb::key_error(std::string(k).c_str());
            return e.attribute(k);
        })
        .def("__setitem__", [](Element &e, std::string_view k, std::string_view v){ e.set_attribute(k, v); })
        .def("__contains__", [](const Element &e, std::string_view k){ return e.has_attribute(k); });

    nb::class_<Document>(m, "Document")
        .def(nb::init<>())
        .def("valid", &Document::valid)
        .def("url", &Document::url)
        .def("set_url", &Document::set_url, nb::arg("url"))
        .def("base_url", &Document::base_url)
        .def("set_base_url", &Document::set_base_url, nb::arg("base_url"))
        .def("title", &Document::title)
        .def("text", &Document::text)
        .def("html", &Document::html)
        .def("root", &Document::root)
        .def("query_selector", &Document::query_selector, nb::arg("selector"))
        .def("query_selector_all", &Document::query_selector_all, nb::arg("selector"))
        .def("get_element_by_id", &Document::get_element_by_id, nb::arg("id"))
        .def("get_elements_by_tag_name", &Document::get_elements_by_tag_name, nb::arg("tag"))
        .def("links", &Document::links)
        .def("forms", &Document::forms)
        .def("scripts", &Document::scripts)
        .def("resource_urls", &Document::resource_urls)
        .def("metadata", &Document::metadata)
        .def("create_element", &Document::create_element, nb::arg("tag"))
        .def("__bool__", &Document::valid)
        .def("__repr__", [](const Document &d){ return d.valid() ? "<Document " + d.title() + ">" : "<Document invalid>"; })
        .def("__len__", [](const Document &d){ return d.valid() ? emit_prowse_dom(d).size() : 0; });

    m.def("parse_html", &parse_html, nb::arg("html"), nb::arg("url") = "", nb::arg("base_url") = "");

    // ---- XPath ----
    nb::enum_<XPathValueType>(m, "XPathValueType")
        .value("NodeSet", XPathValueType::NodeSet)
        .value("String", XPathValueType::String)
        .value("Number", XPathValueType::Number)
        .value("Boolean", XPathValueType::Boolean)
        .export_values();

    nb::class_<XPathValue>(m, "XPathValue")
        .def(nb::init<>())
        .def_rw("type", &XPathValue::type)
        .def_rw("nodes", &XPathValue::nodes)
        .def_rw("string_values", &XPathValue::string_values)
        .def_rw("string_value", &XPathValue::string_value)
        .def_rw("number_value", &XPathValue::number_value)
        .def_rw("boolean_value", &XPathValue::boolean_value)
        .def("__repr__", [](const XPathValue &v){
            if (v.type == XPathValueType::String) return "<XPathValue string=" + v.string_value + ">";
            if (v.type == XPathValueType::Number) return "<XPathValue number=" + std::to_string(v.number_value) + ">";
            if (v.type == XPathValueType::Boolean) return std::string("<XPathValue bool=") + (v.boolean_value ? "true>" : "false>");
            return "<XPathValue nodes=" + std::to_string(v.nodes.size()) + ">";
        });

    m.def("evaluate_xpath", nb::overload_cast<const Document&, std::string_view>(&evaluate_xpath), nb::arg("document"), nb::arg("expression"));
    m.def("evaluate_xpath_element", nb::overload_cast<const Element&, std::string_view>(&evaluate_xpath), nb::arg("element"), nb::arg("expression"));
    m.def("xpath_string_value", &xpath_string_value, nb::arg("document"), nb::arg("expression"));

    // ---- Endpoint extraction ----
    nb::class_<EndpointExtractionOptions>(m, "EndpointExtractionOptions")
        .def(nb::init<>())
        .def_rw("follow_links", &EndpointExtractionOptions::follow_links)
        .def_rw("inspect_scripts", &EndpointExtractionOptions::inspect_scripts)
        .def_rw("observe_network", &EndpointExtractionOptions::observe_network)
        .def_rw("infer_schemas", &EndpointExtractionOptions::infer_schemas)
        .def_rw("include_provenance", &EndpointExtractionOptions::include_provenance)
        .def_rw("redact_secrets", &EndpointExtractionOptions::redact_secrets)
        .def_rw("max_depth", &EndpointExtractionOptions::max_depth)
        .def_rw("max_pages", &EndpointExtractionOptions::max_pages)
        .def_rw("minimum_confidence", &EndpointExtractionOptions::minimum_confidence)
        .def_rw("openapi_version", &EndpointExtractionOptions::openapi_version);

    nb::class_<DiscoveredEndpoint>(m, "DiscoveredEndpoint")
        .def(nb::init<>())
        .def_rw("url", &DiscoveredEndpoint::url)
        .def_rw("path", &DiscoveredEndpoint::path)
        .def_rw("method", &DiscoveredEndpoint::method)
        .def_rw("source", &DiscoveredEndpoint::source)
        .def_rw("discovery_method", &DiscoveredEndpoint::discovery_method)
        .def_rw("confidence", &DiscoveredEndpoint::confidence)
        .def_rw("parameters", &DiscoveredEndpoint::parameters)
        .def_rw("request_content_type", &DiscoveredEndpoint::request_content_type)
        .def_rw("response_content_type", &DiscoveredEndpoint::response_content_type)
        .def_rw("notes", &DiscoveredEndpoint::notes)
        .def("__repr__", [](const DiscoveredEndpoint &e){ return "<Endpoint " + e.method + " " + e.path + ">"; });

    nb::class_<EndpointExtractionResult>(m, "EndpointExtractionResult")
        .def(nb::init<>())
        .def_rw("openapi_yaml", &EndpointExtractionResult::openapi_yaml)
        .def_rw("endpoints", &EndpointExtractionResult::endpoints)
        .def_rw("warnings", &EndpointExtractionResult::warnings);

    nb::class_<EndpointExtractor>(m, "EndpointExtractor")
        .def(nb::init<>())
        .def(nb::init<EndpointExtractionOptions>(), nb::arg("options"))
        .def("extract", &EndpointExtractor::extract, nb::arg("document"))
        .def("observe", &EndpointExtractor::observe, nb::arg("method"), nb::arg("url"), nb::arg("status"), nb::arg("content_type") = "")
        .def("observe_script", &EndpointExtractor::observe_script, nb::arg("source_url"), nb::arg("script_text"))
        .def("set_event_dispatcher", &EndpointExtractor::set_event_dispatcher, nb::arg("dispatcher"))
        .def_prop_ro("options", &EndpointExtractor::options);

    m.def("render_openapi_yaml", &render_openapi_yaml, nb::arg("endpoints"), nb::arg("options"), nb::arg("redactor"));

    // ---- IR ----
    nb::class_<ProwseXasEvent>(m, "ProwseXasEvent")
        .def(nb::init<>())
        .def_rw("kind", &ProwseXasEvent::kind)
        .def_rw("xpath", &ProwseXasEvent::xpath)
        .def_rw("tag", &ProwseXasEvent::tag)
        .def_rw("name", &ProwseXasEvent::name)
        .def_rw("value", &ProwseXasEvent::value)
        .def_rw("depth", &ProwseXasEvent::depth)
        .def("__repr__", [](const ProwseXasEvent &e){ return "<XAS " + e.kind + " " + e.xpath + ">"; });

    nb::class_<ProwseDomNode>(m, "ProwseDomNode")
        .def(nb::init<>())
        .def_rw("xpath", &ProwseDomNode::xpath)
        .def_rw("tag", &ProwseDomNode::tag)
        .def_rw("text", &ProwseDomNode::text)
        .def_rw("attributes", &ProwseDomNode::attributes)
        .def_rw("depth", &ProwseDomNode::depth)
        .def("__repr__", [](const ProwseDomNode &n){ return "<DomNode " + n.tag + " " + n.xpath + ">"; });

    m.def("emit_prowse_xas", &emit_prowse_xas, nb::arg("document"));
    m.def("filter_prowse_xas", &filter_prowse_xas, nb::arg("document"), nb::arg("xpath_expression"));
    m.def("emit_prowse_dom", &emit_prowse_dom, nb::arg("document"));
    m.def("emit_prowse_vtd", &emit_prowse_vtd, nb::arg("document"));
    m.def("decode_prowse_vtd", [](nb::bytes b){
        std::string_view sv(b.c_str(), b.size());
        std::span<const uint8_t> sp(reinterpret_cast<const uint8_t*>(sv.data()), sv.size());
        return decode_prowse_vtd(sp);
    }, nb::arg("bytes"));
    m.def("decode_prowse_vtd_bytes", [](std::vector<uint8_t> v){
        std::span<const uint8_t> sp(v.data(), v.size());
        return decode_prowse_vtd(sp);
    }, nb::arg("bytes"));
    m.def("emit_prowse_iml", &emit_prowse_iml, nb::arg("document"));
    m.def("expand_prowse_iml", &expand_prowse_iml, nb::arg("iml"), nb::arg("expander"));

    // ---- Plugin & Wasm ----
    nb::class_<WasmSandboxConfig>(m, "WasmSandboxConfig")
        .def(nb::init<>())
        .def_rw("component_model", &WasmSandboxConfig::component_model)
        .def_rw("wasi", &WasmSandboxConfig::wasi)
        .def_rw("max_memory_bytes", &WasmSandboxConfig::max_memory_bytes)
        .def_rw("max_table_elements", &WasmSandboxConfig::max_table_elements)
        .def_rw("execution_timeout_ms", &WasmSandboxConfig::execution_timeout_ms)
        .def_rw("fuel", &WasmSandboxConfig::fuel);

    nb::class_<WasmModule>(m, "WasmModule")
        .def("name", &WasmModule::name);

    nb::class_<WasmInstance>(m, "WasmInstance")
        .def("ok", &WasmInstance::ok)
        .def("error", &WasmInstance::error);

    nb::class_<WasmRuntime>(m, "WasmRuntime")
        .def("enabled", &WasmRuntime::enabled)
        .def("capabilities", &WasmRuntime::capabilities)
        .def("load_module", &WasmRuntime::load_module, nb::arg("path"))
        .def("instantiate", &WasmRuntime::instantiate, nb::arg("module"), nb::arg("config"));

    m.def("make_wasm_runtime", &make_wasm_runtime);

    nb::class_<PluginDescriptor>(m, "PluginDescriptor")
        .def(nb::init<>())
        .def_rw("name", &PluginDescriptor::name)
        .def_rw("version", &PluginDescriptor::version)
        .def_rw("abi_version", &PluginDescriptor::abi_version)
        .def_rw("description", &PluginDescriptor::description)
        .def_rw("path", &PluginDescriptor::path)
        .def_rw("lua_module", &PluginDescriptor::lua_module)
        .def_rw("wasm_world", &PluginDescriptor::wasm_world)
        .def_rw("type", &PluginDescriptor::type)
        .def_rw("capabilities", &PluginDescriptor::capabilities)
        .def_rw("initialized", &PluginDescriptor::initialized);

    nb::class_<PluginRegistry>(m, "PluginRegistry")
        .def(nb::init<>())
        .def("load_native", &PluginRegistry::load_native, nb::arg("path"), nb::rv_policy::reference_internal)
        .def("load_lua", &PluginRegistry::load_lua, nb::arg("path"), nb::rv_policy::reference_internal)
        .def("load_wasm", &PluginRegistry::load_wasm, nb::arg("path"), nb::arg("config") = WasmSandboxConfig{}, nb::rv_policy::reference_internal)
        .def("discover", nb::overload_cast<const std::filesystem::path&>(&PluginRegistry::discover), nb::arg("directory"))
        .def("initialize_all", &PluginRegistry::initialize_all)
        .def("configure_all", &PluginRegistry::configure_all, nb::arg("entries") = std::vector<std::pair<std::string,std::string>>{})
        .def("dispatch_before_request", &PluginRegistry::dispatch_before_request, nb::arg("request"))
        .def("dispatch_after_response", &PluginRegistry::dispatch_after_response, nb::arg("request"), nb::arg("response"))
        .def("dispatch_document", &PluginRegistry::dispatch_document, nb::arg("document"))
        .def("shutdown_all", &PluginRegistry::shutdown_all)
        .def("plugins", &PluginRegistry::plugins)
        .def("find", [](const PluginRegistry &r, std::string_view n) -> const PluginDescriptor* { return r.find(n); }, nb::rv_policy::reference_internal)
        .def("has_capability", &PluginRegistry::has_capability, nb::arg("capability"));

    // ---- BrowserConfig / SessionConfig / Browser / Session ----
    nb::class_<BrowserConfig>(m, "BrowserConfig")
        .def(nb::init<>())
        .def_rw("user_agent", &BrowserConfig::user_agent)
        .def_rw("javascript", &BrowserConfig::javascript)
        .def_rw("follow_redirects", &BrowserConfig::follow_redirects)
        .def_rw("max_redirects", &BrowserConfig::max_redirects)
        .def_rw("timeout_ms", &BrowserConfig::timeout_ms)
        .def_rw("max_response_bytes", &BrowserConfig::max_response_bytes)
        .def_rw("unsupported_api_behavior", &BrowserConfig::unsupported_api_behavior)
        .def_rw("observe_network", &BrowserConfig::observe_network)
        .def_rw("redaction", &BrowserConfig::redaction);

    nb::class_<SessionConfig>(m, "SessionConfig")
        .def(nb::init<>())
        .def_rw("profile", &SessionConfig::profile)
        .def_rw("isolate_storage", &SessionConfig::isolate_storage)
        .def_rw("persist_session", &SessionConfig::persist_session)
        .def_rw("reuse_cookies", &SessionConfig::reuse_cookies);

    nb::class_<Browser>(m, "Browser")
        .def(nb::init<BrowserConfig>(), nb::arg("config") = BrowserConfig{})
        .def("create_session", &Browser::create_session, nb::arg("config") = SessionConfig{}, nb::keep_alive<0, 1>())
        .def("set_network_client", [](Browser &b, MemoryNetworkClient* c){
            struct Forward : NetworkClient {
                MemoryNetworkClient* impl;
                explicit Forward(MemoryNetworkClient* p) : impl(p) {}
                HttpResponse send(const HttpRequest& r) override { return impl->send(r); }
                std::string name() const override { return impl->name(); }
            };
            b.set_network_client(std::make_unique<Forward>(c));
        }, nb::arg("client"), nb::keep_alive<1, 2>())
        .def("set_network_client_generic", [](Browser &b, std::unique_ptr<NetworkClient> c){ b.set_network_client(std::move(c)); }, nb::arg("client"))
        .def("network_client", nb::overload_cast<>(&Browser::network_client), nb::rv_policy::reference_internal)
        .def("storage", nb::overload_cast<>(&Browser::storage), nb::rv_policy::reference_internal)
        .def("events", nb::overload_cast<>(&Browser::events), nb::rv_policy::reference_internal)
        .def("plugins", nb::overload_cast<>(&Browser::plugins), nb::rv_policy::reference_internal)
        .def("wasm", nb::overload_cast<>(&Browser::wasm), nb::rv_policy::reference_internal)
        .def("lua", &Browser::lua, nb::rv_policy::reference)
        .def("create_javascript_runtime", &Browser::create_javascript_runtime)
        .def("capabilities", &Browser::capabilities)
        .def("web_platform", &Browser::web_platform)
        .def("handle_unsupported_api", &Browser::handle_unsupported_api, nb::arg("name"), nb::arg("message"))
        .def_prop_ro("config", [](const Browser &b) -> const BrowserConfig& { return b.config(); })
        .def("__enter__", [](Browser &b){ return &b; }, nb::rv_policy::reference)
        .def("__exit__", [](Browser &, nb::handle, nb::handle, nb::handle){ return false; });

    nb::class_<Session>(m, "Session")
        .def("id", &Session::id)
        .def("browser", nb::overload_cast<>(&Session::browser), nb::rv_policy::reference_internal)
        .def("navigate", &Session::navigate, nb::arg("url"))
        .def("load_html", &Session::load_html, nb::arg("html"), nb::arg("base_url") = "")
        .def("document", &Session::document)
        .def_prop_ro("current_url", [](const Session &s){ return s.current_url(); })
        .def("set_header", &Session::set_header, nb::arg("name"), nb::arg("value"))
        .def("clear_headers", &Session::clear_headers)
        .def("headers", &Session::headers)
        .def("evaluate_js", &Session::evaluate_js, nb::arg("script"), nb::arg("options") = ScriptOptions{})
        .def("request", &Session::request, nb::arg("request"))
        .def("cookies", nb::overload_cast<>(&Session::cookies), nb::rv_policy::reference_internal)
        .def("local_storage", nb::overload_cast<>(&Session::local_storage), nb::rv_policy::reference_internal)
        .def("session_storage", nb::overload_cast<>(&Session::session_storage), nb::rv_policy::reference_internal)
        .def("capabilities", &Session::capabilities)
        .def("events", nb::overload_cast<>(&Session::events), nb::rv_policy::reference_internal)
        .def("close", &Session::close)
        .def("__enter__", [](Session &s){ return &s; }, nb::rv_policy::reference)
        .def("__exit__", [](Session &s, nb::handle, nb::handle, nb::handle){ s.close(); return false; })
        .def("__repr__", [](const Session &s){ return "<Session " + s.id() + " " + s.current_url() + ">"; });

    // ---- ProjectConfig ----
    nb::class_<ConfigArgument>(m, "ConfigArgument")
        .def(nb::init<>())
        .def_rw("name", &ConfigArgument::name)
        .def_rw("type", &ConfigArgument::type)
        .def_rw("required", &ConfigArgument::required)
        .def_rw("secret", &ConfigArgument::secret)
        .def_rw("default_value", &ConfigArgument::default_value);
    nb::class_<DriverConfig>(m, "DriverConfig")
        .def(nb::init<>())
        .def_rw("name", &DriverConfig::name)
        .def_rw("description", &DriverConfig::description)
        .def_rw("script", &DriverConfig::script)
        .def_rw("entrypoint", &DriverConfig::entrypoint)
        .def_rw("enabled", &DriverConfig::enabled)
        .def_rw("arguments", &DriverConfig::arguments);
    nb::class_<ExtensionConfig>(m, "ExtensionConfig")
        .def(nb::init<>())
        .def_rw("name", &ExtensionConfig::name)
        .def_rw("description", &ExtensionConfig::description)
        .def_rw("type", &ExtensionConfig::type)
        .def_rw("module", &ExtensionConfig::module)
        .def_rw("enabled", &ExtensionConfig::enabled)
        .def_rw("autoload", &ExtensionConfig::autoload)
        .def_rw("required_modules", &ExtensionConfig::required_modules)
        .def_rw("events", &ExtensionConfig::events);
    nb::class_<PluginConfig>(m, "PluginConfig")
        .def(nb::init<>())
        .def_rw("name", &PluginConfig::name)
        .def_rw("description", &PluginConfig::description)
        .def_rw("type", &PluginConfig::type)
        .def_rw("path", &PluginConfig::path)
        .def_rw("enabled", &PluginConfig::enabled)
        .def_rw("autoload", &PluginConfig::autoload)
        .def_rw("capabilities", &PluginConfig::capabilities)
        .def_rw("lua_modules", &PluginConfig::lua_modules);
    nb::class_<PluginSandboxConfig>(m, "PluginSandboxConfig")
        .def(nb::init<>())
        .def_rw("component", &PluginSandboxConfig::component)
        .def_rw("wasi", &PluginSandboxConfig::wasi)
        .def_rw("max_memory_mb", &PluginSandboxConfig::max_memory_mb)
        .def_rw("execution_timeout_ms", &PluginSandboxConfig::execution_timeout_ms)
        .def_rw("filesystem", &PluginSandboxConfig::filesystem)
        .def_rw("network", &PluginSandboxConfig::network);
    nb::class_<CommandConfig>(m, "CommandConfig")
        .def(nb::init<>())
        .def_rw("name", &CommandConfig::name)
        .def_rw("description", &CommandConfig::description)
        .def_rw("handler", &CommandConfig::handler)
        .def_rw("driver", &CommandConfig::driver)
        .def_rw("output", &CommandConfig::output)
        .def_rw("arguments", &CommandConfig::arguments);
    nb::class_<EndpointExtractionConfig>(m, "EndpointExtractionConfig")
        .def(nb::init<>())
        .def_rw("enabled", &EndpointExtractionConfig::enabled)
        .def_rw("plugin", &EndpointExtractionConfig::plugin)
        .def_rw("base_urls", &EndpointExtractionConfig::base_urls)
        .def_rw("follow_links", &EndpointExtractionConfig::follow_links)
        .def_rw("inspect_forms", &EndpointExtractionConfig::inspect_forms)
        .def_rw("inspect_inline_scripts", &EndpointExtractionConfig::inspect_inline_scripts)
        .def_rw("inspect_external_scripts", &EndpointExtractionConfig::inspect_external_scripts)
        .def_rw("observe_network", &EndpointExtractionConfig::observe_network)
        .def_rw("inspect_json_config", &EndpointExtractionConfig::inspect_json_config)
        .def_rw("max_depth", &EndpointExtractionConfig::max_depth)
        .def_rw("max_pages", &EndpointExtractionConfig::max_pages)
        .def_rw("max_requests", &EndpointExtractionConfig::max_requests)
        .def_rw("same_origin_only", &EndpointExtractionConfig::same_origin_only)
        .def_rw("allowed_hosts", &EndpointExtractionConfig::allowed_hosts)
        .def_rw("blocked_path_patterns", &EndpointExtractionConfig::blocked_path_patterns)
        .def_rw("infer_parameters", &EndpointExtractionConfig::infer_parameters)
        .def_rw("infer_request_bodies", &EndpointExtractionConfig::infer_request_bodies)
        .def_rw("infer_response_schemas", &EndpointExtractionConfig::infer_response_schemas)
        .def_rw("infer_status_codes", &EndpointExtractionConfig::infer_status_codes)
        .def_rw("minimum_confidence", &EndpointExtractionConfig::minimum_confidence)
        .def_rw("openapi_version", &EndpointExtractionConfig::openapi_version)
        .def_rw("output", &EndpointExtractionConfig::output)
        .def_rw("include_examples", &EndpointExtractionConfig::include_examples)
        .def_rw("include_provenance", &EndpointExtractionConfig::include_provenance)
        .def_rw("redact_cookies", &EndpointExtractionConfig::redact_cookies)
        .def_rw("redact_authorization_headers", &EndpointExtractionConfig::redact_authorization_headers)
        .def_rw("redact_query_parameters", &EndpointExtractionConfig::redact_query_parameters);
    nb::class_<SecurityConfig>(m, "SecurityConfig")
        .def(nb::init<>())
        .def_rw("allowed_schemes", &SecurityConfig::allowed_schemes)
        .def_rw("allow_file_urls", &SecurityConfig::allow_file_urls)
        .def_rw("allow_private_networks", &SecurityConfig::allow_private_networks)
        .def_rw("allow_loopback", &SecurityConfig::allow_loopback)
        .def_rw("allow_native_lua", &SecurityConfig::allow_native_lua)
        .def_rw("allow_dynamic_plugin_loading", &SecurityConfig::allow_dynamic_plugin_loading);
    nb::class_<SessionDefaultsConfig>(m, "SessionDefaultsConfig")
        .def(nb::init<>())
        .def_rw("default_profile", &SessionDefaultsConfig::default_profile)
        .def_rw("reuse_cookies", &SessionDefaultsConfig::reuse_cookies)
        .def_rw("persist_session", &SessionDefaultsConfig::persist_session)
        .def_rw("isolate_storage", &SessionDefaultsConfig::isolate_storage);
    nb::class_<ProjectConfig>(m, "ProjectConfig")
        .def(nb::init<>())
        .def_rw("name", &ProjectConfig::name)
        .def_rw("version", &ProjectConfig::version)
        .def_rw("description", &ProjectConfig::description)
        .def_rw("root", &ProjectConfig::root)
        .def_rw("user_agent", &ProjectConfig::user_agent)
        .def_rw("javascript", &ProjectConfig::javascript)
        .def_rw("follow_redirects", &ProjectConfig::follow_redirects)
        .def_rw("max_redirects", &ProjectConfig::max_redirects)
        .def_rw("timeout_ms", &ProjectConfig::timeout_ms)
        .def_rw("unsupported_api_behavior", &ProjectConfig::unsupported_api_behavior)
        .def_rw("observe_network", &ProjectConfig::observe_network)
        .def_rw("lua_version", &ProjectConfig::lua_version)
        .def_rw("lua_libraries", &ProjectConfig::lua_libraries)
        .def_rw("lua_preload", &ProjectConfig::lua_preload)
        .def_rw("allow_extensions", &ProjectConfig::allow_extensions)
        .def_rw("lua_max_memory_mb", &ProjectConfig::lua_max_memory_mb)
        .def_rw("lua_execution_timeout_ms", &ProjectConfig::lua_execution_timeout_ms)
        .def_rw("network_proxy", &ProjectConfig::network_proxy)
        .def_rw("drivers", &ProjectConfig::drivers)
        .def_rw("extensions", &ProjectConfig::extensions)
        .def_rw("plugins", &ProjectConfig::plugins)
        .def_rw("plugin_config", &ProjectConfig::plugin_config)
        .def_rw("commands", &ProjectConfig::commands)
        .def_rw("endpoint_extraction", &ProjectConfig::endpoint_extraction)
        .def_rw("security", &ProjectConfig::security)
        .def_rw("sessions", &ProjectConfig::sessions)
        .def_rw("variables", &ProjectConfig::variables);

    m.def("load_project_config", &load_project_config, nb::arg("path"));
    m.def("parse_project_config", &parse_project_config, nb::arg("contents"));

    // ---- WebInterface ----
    nb::class_<WebRequest>(m, "WebRequest")
        .def(nb::init<>())
        .def_rw("method", &WebRequest::method)
        .def_rw("path", &WebRequest::path)
        .def_rw("query", &WebRequest::query)
        .def_rw("headers", &WebRequest::headers)
        .def_rw("body", &WebRequest::body);

    nb::class_<WebResponse>(m, "WebResponse")
        .def(nb::init<>())
        .def_rw("status", &WebResponse::status)
        .def_rw("headers", &WebResponse::headers)
        .def_rw("body", &WebResponse::body)
        .def_static("json", &WebResponse::json, nb::arg("status"), nb::arg("body"))
        .def_static("text", &WebResponse::text, nb::arg("status"), nb::arg("body"), nb::arg("content_type") = "text/plain; charset=utf-8")
        .def_static("not_found", &WebResponse::not_found)
        .def_static("method_not_allowed", &WebResponse::method_not_allowed);

    nb::class_<WebInterfaceConfig>(m, "WebInterfaceConfig")
        .def(nb::init<>())
        .def_rw("browser", &WebInterfaceConfig::browser)
        .def_rw("web_root", &WebInterfaceConfig::web_root)
        .def_rw("enable_playwright", &WebInterfaceConfig::enable_playwright);

    nb::class_<WebInterface>(m, "WebInterface")
        .def(nb::init<WebInterfaceConfig>(), nb::arg("config") = WebInterfaceConfig{})
        .def("handle", &WebInterface::handle, nb::arg("request"))
        .def("browser", nb::overload_cast<>(&WebInterface::browser), nb::rv_policy::reference_internal)
        .def("web_root", [](const WebInterface &w){ return w.web_root().string(); });

    // ---- Convenience free helpers ----
    m.def("parse_html_document", [](std::string_view html, std::string url, std::string base){
        return parse_html(html, url, base);
    }, nb::arg("html"), nb::arg("url") = "", nb::arg("base_url") = "");
}
