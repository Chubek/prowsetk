#ifndef PROWSETK_PLUGINS_EBPF_INTERFACE_HPP
#define PROWSETK_PLUGINS_EBPF_INTERFACE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace prowsetk::plugins::ebpf_interface {

struct EndpointObservation {
    std::string method;
    std::string url;
    int status = 0;
    std::string content_type;
};

class EndpointCollector {
public:
    explicit EndpointCollector(std::size_t max_records = 4096,
                                bool redact_secrets = true);
    ~EndpointCollector();
    EndpointCollector(EndpointCollector&&) noexcept;
    EndpointCollector& operator=(EndpointCollector&&) noexcept;
    EndpointCollector(const EndpointCollector&) = delete;
    EndpointCollector& operator=(const EndpointCollector&) = delete;
    void observe_request(std::string_view method, std::string_view url);
    void observe_response(std::string_view url, int status,
                          std::string_view content_type);
    void clear();
    const std::vector<EndpointObservation>& observations() const noexcept;
    std::string render_json() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct RuntimeCapabilities {
    bool libbpf = false;
    bool bpf_object = false;
    bool bpf_program = false;
    bool bpf_map = false;
    bool bpf_link = false;
    bool ring_buffer = false;
    bool perf_buffer = false;
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    RuntimeCapabilities capabilities() const noexcept;
    bool available() const noexcept;
    bool load_object(std::string_view path, std::string* error = nullptr);
    bool attach_all(std::string* error = nullptr);
    std::size_t poll(int timeout_ms, std::string* error = nullptr);
    void detach();
    std::vector<std::string> map_names() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace prowsetk::plugins::ebpf_interface

#endif
