#include <prowsetk/plugins/basic_gui.hpp>

#include <fstream>
#include <utility>

#include <prowsetk/error.hpp>
#include <prowsetk/lua_runtime.hpp>
#include <prowsetk/plugins/opencode_marionette.hpp>

namespace prowsetk::basic_gui {
namespace {
constexpr std::size_t max_lua_bytes = 64u * 1024u;
struct NavigationGuard {
    EventDispatcher& events;
    SubscriptionId id;
    ~NavigationGuard() { events.unsubscribe(id); }
};
struct RunningGuard {
    bool& running;
    explicit RunningGuard(bool& value) : running(value) {
        if (std::exchange(running, true))
            throw Error(ErrorCode::InvalidArgument, "marionette already running");
    }
    ~RunningGuard() { running = false; }
};
}

MarionetteResult Controller::run_marionette(std::string_view source, std::string_view goal,
                                          NetworkClient* agent_transport) {
    if (!LuaRuntime::available()) throw Error(ErrorCode::Unsupported, "Lua unavailable");
    if (source.size() > max_lua_bytes || goal.size() > 4096)
        throw Error(ErrorCode::ResourceLimit, "marionette input limit");
    RunningGuard guard(marionette_running_);
    remember_navigation();
    NavigationGuard navigation{session_.events(), session_.events().subscribe(
        EventType::AfterNavigation, [this](Event& event) {
            if (event.session_id == session_.id()) remember_navigation();
        })};
    const auto previous = session_.document();
    const auto previous_url = session_.current_url();
    auto synchronize = [&] {
        if (session_.current_url() != previous_url) {
            offline_html_.reset();
            remember_navigation();
        } else if (session_.document() != previous) {
            // Lua preparation can replace the offline document at the same URL.
            offline_html_.reset();
        }
        refresh();
    };
    try {
        MarionetteResult result;
        {
            LuaRuntime lua;
            lua.bind_browser(&session_.browser());
            // The Controller's documented borrow keeps the session alive for
            // this call. All managed Lua copies and subscriptions are destroyed
            // before returning; this handle cannot escape into another runtime.
            lua.bind_session(std::shared_ptr<Session>(&session_, [](Session*) {}));
            // Routine print output must not copy page values to host stdout.
            if (!lua.run("print = function(...) end").ok || !lua.run(source, "marionette").ok)
                throw Error(ErrorCode::LuaError, "marionette script failed");
            // Validate the return inside a protected Lua call. In particular,
            // never ask the general call_function API to stringify arbitrary
            // userdata/tables with a caller-controlled __tostring metamethod.
            if (!lua.run(R"lua(
                local entry, value_type, fail = main, type, error
                function __prowsetk_gui_main(args)
                    if value_type(entry) ~= 'function' then fail('missing main') end
                    local policy = entry(args)
                    if value_type(policy) ~= 'string' then fail('policy must be JSON string') end
                    return policy
                end
            )lua").ok) throw Error(ErrorCode::LuaError, "marionette entrypoint failed");
            std::string json;
            if (!lua.call_function("__prowsetk_gui_main", {{"goal", "string", std::string(goal)}}, &json).ok)
                throw Error(ErrorCode::LuaError, "marionette entrypoint failed");
            namespace marionette = plugins::opencode_marionette;
            namespace bridge = plugins::opencode_bridge;
            auto policy = marionette::parse_decisions(json);
            if (!goal.empty()) policy.goal = goal;
            auto config = bridge::config_from_environment();
            config.api_prefix = "/api";
            config.max_requests = 512;
            // Agent IPC never uses page cookies, headers or the page transport.
            auto network = agent_transport ? nullptr : make_socket_network_client();
            bridge::OpenCodeClient client(agent_transport ? *agent_transport : *network, config);
            const auto run = marionette::run(session_, client, policy);
            result = {run.steps, run.stopped, run.reason};
        }
        synchronize();
        return result;
    } catch (...) {
        // Actions are immediate, including actions before a failed reply. Keep
        // the inspector and history consistent, and preserve the original error.
        try { synchronize(); } catch (...) {}
        throw;
    }
}

MarionetteResult Controller::run_marionette_file(const std::filesystem::path& path,
                                               std::string_view goal) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Error(ErrorCode::IoError, "marionette file unavailable");
    // Read one extra byte to reject growth and nonregular oversized streams.
    std::string source(max_lua_bytes + 1, '\0');
    file.read(source.data(), static_cast<std::streamsize>(source.size()));
    const auto size = static_cast<std::size_t>(file.gcount());
    if (size > max_lua_bytes) throw Error(ErrorCode::ResourceLimit, "marionette input limit");
    if (file.bad() || (!file.eof() && file.fail()))
        throw Error(ErrorCode::IoError, "marionette file read failed");
    source.resize(size);
    return run_marionette(source, goal);
}
}  // namespace prowsetk::basic_gui
