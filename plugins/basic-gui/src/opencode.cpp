#include <prowsetk/plugins/basic_gui.hpp>
#include <prowsetk/error.hpp>
#include <prowsetk/url.hpp>

#include <utility>
#include "opencode_config.hpp"

namespace prowsetk::basic_gui {
namespace bridge = plugins::opencode_bridge;
namespace detail {
bridge::BridgeConfig opencode_config(std::string_view base_url, std::size_t requests) {
    auto config = bridge::config_from_environment();
    if (!base_url.empty()) config.base_url = base_url;
    config.api_prefix = "/api";
    config.max_requests = requests;
    bridge::validate_config(config);
    return config;
}
}
namespace {
struct AgentGuard {
    bool& active;
    explicit AgentGuard(bool& value) : active(value) {
        if (std::exchange(active, true))
            throw Error(ErrorCode::InvalidArgument, "OpenCode operation already active");
    }
    ~AgentGuard() { active = false; }
};
}

void Controller::set_opencode_base_url(std::string_view url) {
    if (marionette_running_) throw Error(ErrorCode::InvalidArgument, "OpenCode operation already active");
    if (url.size() > 4096) throw Error(ErrorCode::ResourceLimit, "OpenCode URL limit");
    // Validate before changing the current configuration. The bridge rejects
    // userinfo, queries, fragments, remote HTTP and unsupported URL schemes.
    detail::opencode_config(url);
    opencode_base_url_ = url;
}

void Controller::check_opencode(NetworkClient* transport) {
    AgentGuard guard(marionette_running_);
    auto config = detail::opencode_config(opencode_base_url_, 1);
    auto network = transport ? nullptr : make_socket_network_client();
    bridge::OpenCodeClient client(transport ? *transport : *network, std::move(config));
    // This is an explicit API/authentication check, creating a decision-only
    // agent session. It neither loads nor acts on the displayed page.
    client.create_session(true);
}

std::string Controller::ask_opencode(std::string_view prompt, NetworkClient* transport) {
    if (prompt.empty()) throw Error(ErrorCode::InvalidArgument, "OpenCode prompt required");
    if (prompt.size() > 4096) throw Error(ErrorCode::ResourceLimit, "OpenCode prompt limit");
    AgentGuard guard(marionette_running_);
    if (!session_.document()) throw Error(ErrorCode::InvalidArgument, "Load a page first");
    refresh();
    if (snapshot_.limited) throw Error(ErrorCode::ResourceLimit, "OpenCode snapshot limit");
    auto config = detail::opencode_config(opencode_base_url_);
    // Send structural inspection context only: no text, attribute values,
    // source, scripts, credentials, URL queries or fragments.
    auto url = parse_url(session_.current_url());
    url.userinfo.clear(); url.query.clear(); url.has_query = false;
    url.fragment.clear(); url.has_fragment = false;
    std::string context = "Advise the user about the page structure and permitted automation. "
        "Do not execute tools or return executable code.\nUser prompt: " + std::string(prompt) +
        "\nPage URL: " + url.to_string() + "\nElements (canonical path, tag, state):\n";
    for (const auto& node : snapshot_.nodes) {
        if (node.tag == "script" || node.tag == "style") continue;
        context += node.path + " tag=" + node.tag;
        if (node.hidden) context += " hidden";
        if (node.disabled) context += " disabled";
        context += '\n';
        if (context.size() > config.max_input_bytes)
            throw Error(ErrorCode::ResourceLimit, "OpenCode context limit");
    }
    auto network = transport ? nullptr : make_socket_network_client();
    bridge::OpenCodeClient client(transport ? *transport : *network, std::move(config));
    const auto id = client.create_session(true);
    const auto answer = client.prompt(id, context);
    return show_values_ ? answer.substr(0, 4096)
        : "[OpenCode reply hidden; enable Console values to view subsequent replies]";
}
}  // namespace prowsetk::basic_gui
