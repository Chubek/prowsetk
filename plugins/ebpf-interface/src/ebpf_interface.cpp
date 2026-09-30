#include "prowsetk/plugins/ebpf_interface.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

#ifdef PROWSETK_HAVE_LIBBPF
#if __has_include(<bpf/libbpf.h>)
#include <bpf/libbpf.h>
#else
#include <libbpf.h>
#endif
#endif

namespace prowsetk::plugins::ebpf_interface {
namespace {
std::string json_escape(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) { case '\\': out += "\\\\"; break; case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break; case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break; default: out += c; }
    }
    return out;
}
std::string redact_url(std::string_view url) {
    const auto q = url.find('?');
    if (q == std::string_view::npos) return std::string(url);
    std::string out(url.substr(0, q + 1));
    bool first = true;
    for (std::size_t p = q + 1; p <= url.size();) {
        const auto e = url.find('&', p);
        const auto part = url.substr(p, e == std::string_view::npos ? url.size() - p : e - p);
        const auto eq = part.find('=');
        const auto key = part.substr(0, eq);
        const bool secret = key == "token" || key == "access_token" || key == "api_key" ||
                            key == "apikey" || key == "password" || key == "secret" ||
                            key == "session" || key == "sessionid" || key == "auth";
        if (!first) out += '&';
        first = false;
        out += key;
        if (eq != std::string_view::npos) out += secret ? "=[REDACTED]" : "=" + std::string(part.substr(eq + 1));
        if (e == std::string_view::npos) break;
        p = e + 1;
    }
    return out;
}
}

struct EndpointCollector::Impl { std::size_t max_records; bool redact; std::vector<EndpointObservation> observations; };
EndpointCollector::EndpointCollector(std::size_t max_records, bool redact)
    : impl_(std::make_unique<Impl>(Impl{max_records, redact, {}})) {}
EndpointCollector::~EndpointCollector() = default;
EndpointCollector::EndpointCollector(EndpointCollector&&) noexcept = default;
EndpointCollector& EndpointCollector::operator=(EndpointCollector&&) noexcept = default;
void EndpointCollector::observe_request(std::string_view method, std::string_view url) {
    if (impl_->observations.size() >= impl_->max_records) return;
    impl_->observations.push_back({std::string(method), impl_->redact ? redact_url(url) : std::string(url), 0, {}});
}
void EndpointCollector::observe_response(std::string_view url, int status, std::string_view content_type) {
    const auto normalized = impl_->redact ? redact_url(url) : std::string(url);
    for (auto it = impl_->observations.rbegin(); it != impl_->observations.rend(); ++it) {
        if (it->url == normalized && it->status == 0) { it->status = status; it->content_type = content_type; return; }
    }
    if (impl_->observations.size() < impl_->max_records) impl_->observations.push_back({"GET", normalized, status, std::string(content_type)});
}
void EndpointCollector::clear() { impl_->observations.clear(); }
const std::vector<EndpointObservation>& EndpointCollector::observations() const noexcept { return impl_->observations; }
std::string EndpointCollector::render_json() const {
    std::ostringstream out; out << "{\"observations\":[";
    for (std::size_t i = 0; i < impl_->observations.size(); ++i) {
        if (i) out << ',';
        const auto& e = impl_->observations[i];
        out << "{\"method\":\"" << json_escape(e.method) << "\",\"url\":\""
            << json_escape(e.url) << "\",\"status\":" << e.status
            << ",\"content_type\":\"" << json_escape(e.content_type) << "\"}";
    }
    out << "]}"; return out.str();
}

struct Runtime::Impl {
    bool loaded = false;
    std::string path;
    std::string error;
#ifdef PROWSETK_HAVE_LIBBPF
    bpf_object* object = nullptr;
    std::vector<bpf_link*> links;
#endif
};
Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() { detach(); }
RuntimeCapabilities Runtime::capabilities() const noexcept {
#ifdef PROWSETK_HAVE_LIBBPF
    return {true, true, true, true, true, true, true};
#else
    return {};
#endif
}
bool Runtime::available() const noexcept { return capabilities().libbpf; }
bool Runtime::load_object(std::string_view path, std::string* error) {
#ifdef PROWSETK_HAVE_LIBBPF
    detach();
    bpf_object* object = bpf_object__open_file(std::string(path).c_str(), nullptr);
    if (libbpf_get_error(object)) { if (error) *error = "libbpf failed to open object"; return false; }
    if (bpf_object__load(object) != 0) { bpf_object__close(object); if (error) *error = "libbpf failed to load object"; return false; }
    impl_->object = object; impl_->loaded = true; impl_->path = path; return true;
#else
    if (error) *error = "libbpf support is disabled; configure with PROWSETK_ENABLE_EBPF=ON";
    (void)path; return false;
#endif
}
bool Runtime::attach_all(std::string* error) {
    if (!impl_->loaded) { if (error) *error = "no eBPF object loaded"; return false; }
#ifdef PROWSETK_HAVE_LIBBPF
    bpf_program* program = nullptr;
    bpf_object__for_each_program(program, impl_->object) {
        bpf_link* link = bpf_program__attach(program);
        if (libbpf_get_error(link)) {
            if (error) *error = "libbpf failed to attach a program";
            detach();
            return false;
        }
        impl_->links.push_back(link);
    }
#endif
    return true;
}
std::size_t Runtime::poll(int timeout_ms, std::string* error) { (void)timeout_ms; if (!available()) { if (error) *error = "libbpf unavailable"; return 0; } return 0; }
void Runtime::detach() {
#ifdef PROWSETK_HAVE_LIBBPF
    for (bpf_link* link : impl_->links) bpf_link__destroy(link);
    impl_->links.clear();
    if (impl_->object) bpf_object__close(impl_->object);
    impl_->object = nullptr;
#endif
    impl_->loaded = false;
}
std::vector<std::string> Runtime::map_names() const {
    std::vector<std::string> names;
#ifdef PROWSETK_HAVE_LIBBPF
    if (impl_->object) {
        bpf_map* map = nullptr;
        bpf_object__for_each_map(map, impl_->object) {
            const char* name = bpf_map__name(map);
            if (name) names.emplace_back(name);
        }
    }
#endif
    return names;
}
}
