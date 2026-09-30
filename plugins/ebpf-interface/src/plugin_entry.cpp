#include "prowsetk/ProwseTk-Plugin.h"
#include "prowsetk/plugins/ebpf_interface.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>

namespace {
using prowsetk::plugins::ebpf_interface::EndpointCollector;
struct Config { EndpointCollector collector; std::filesystem::path output; Config() : collector(4096, true) {} };
std::mutex mutex; std::map<ProwseTkHost*, Config> states;
const char* caps[] = {"ebpf", "libbpf", "endpoint-observation", "network-tracing", "bpf-object", "bpf-program", "bpf-map", "bpf-link", "ring-buffer", "perf-buffer"};
const ProwseTkPluginInfo info = {"ebpf-interface", "0.1.0", "2", "Optional libbpf/eBPF interface and host-mediated endpoint collector", PROWSETK_PLUGIN_NATIVE, "ebpf_interface", "prowsetk-plugin", caps, 10};
int initialize(ProwseTkHost* h) noexcept { if (!h || !h->api || h->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION) return PROWSETK_STATUS_INVALID_ARGUMENT; std::lock_guard lock(mutex); states.try_emplace(h); return 0; }
void shutdown(ProwseTkHost* h) noexcept { std::lock_guard lock(mutex); states.erase(h); }
int configure(ProwseTkHost* h, const ProwseTkConfigEntry* e, size_t n) noexcept { if (!h || (n && !e)) return PROWSETK_STATUS_INVALID_ARGUMENT; std::lock_guard lock(mutex); auto it=states.find(h); if(it==states.end()) return PROWSETK_STATUS_INVALID_ARGUMENT; for(size_t i=0;i<n;++i){ if(!e[i].key||!e[i].value) return PROWSETK_STATUS_INVALID_ARGUMENT; std::string_view k=e[i].key,v=e[i].value; if(k=="ebpf-interface.output"||k=="ebpf.output") it->second.output=v; else if(k=="ebpf-interface.max_records"){ try { it->second.collector = EndpointCollector(static_cast<size_t>(std::stoul(std::string(v))), true); } catch(...) { return PROWSETK_STATUS_INVALID_ARGUMENT; } } } return 0; }
int before_request(ProwseTkHost* h, const ProwseTkHttpRequest* r, ProwseTkHookResult*) noexcept { if(!h||!r||!r->url) return PROWSETK_STATUS_INVALID_ARGUMENT; std::lock_guard lock(mutex); auto it=states.find(h); if(it==states.end()) return 2; it->second.collector.observe_request(r->method?r->method:"GET", r->url); return 0; }
int after_response(ProwseTkHost* h, const ProwseTkHttpRequest* r, const ProwseTkHttpResponse* s, ProwseTkHookResult*) noexcept { if(!h||!r||!s) return PROWSETK_STATUS_INVALID_ARGUMENT; std::lock_guard lock(mutex); auto it=states.find(h); if(it==states.end()) return 2; std::string ct; for(size_t i=0;i<s->header_count;++i) if(s->headers[i].name&&s->headers[i].value && std::string_view(s->headers[i].name)=="content-type") ct=s->headers[i].value; it->second.collector.observe_response(r->url?r->url:"", s->status, ct); if(!it->second.output.empty()){ std::ofstream f(it->second.output); if(f) f << it->second.collector.render_json(); } return 0; }
const ProwseTkPlugin plugin = {initialize, shutdown, [](){return &info;}, configure, before_request, after_response, nullptr};
}
extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) { return &plugin; }
