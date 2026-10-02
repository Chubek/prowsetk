#include "pagewatch.hpp"
#include "lua_sources.hpp"
#include <algorithm>
#include <csignal>
#include <poll.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace prowsetk::pagewatch {
namespace {
volatile std::sig_atomic_t stop_worker = 0, force_sample = 0;
void stop(int) { stop_worker = 1; }
void force(int) { force_sample = 1; }
}
int worker(const std::filesystem::path& directory, int ipc_fd) {
    automation::Fd ipc(ipc_fd);
    automation::socket_deadline(ipc.get(), 2000);
    // A runaway Lua coroutine cannot observe a cooperative stop flag.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() == 1) return 1;
    std::signal(SIGTERM, stop); std::signal(SIGINT, stop); std::signal(SIGUSR1, force);
    try {
        const auto config = parse_watch_config(automation::read_file(directory / "Watcher.toml", script_limit));
        Browser browser(config.browser);
        // A fresh quota for every polling iteration; redirects and page scripts
        // cannot bypass the same-origin transport boundary.
        auto reset_network = [&]() {
            browser.set_network_client(std::make_unique<automation::RestrictedNetwork>(
                config.html_file.empty() ? std::vector<std::string>{automation::origin(config.url)} : std::vector<std::string>{}, 128));
        };
        reset_network();
        auto session = browser.create_session();
        LuaRuntime lua;
        lua.bind_browser(&browser); lua.bind_session(session);
        automation::load_module(lua, "lpgwatch", automation::lpgwatch_source);
        if (!lua.run(automation::read_file(directory / "watcher.lua", script_limit), "pagewatch-driver").ok ||
            automation::call(lua, "main", config.arguments) != "0") throw std::runtime_error("watcher initialization failed");
        std::string previous;
        if (std::filesystem::exists(directory / "tab.json")) previous = automation::read_file(directory / "tab.json", sample_limit);
        auto next_sample = automation::Clock::now();
        while (!stop_worker) {
            const auto now = automation::Clock::now();
            if (force_sample || now >= next_sample) {
                force_sample = 0;
                try {
                    reset_network();
                    if (!config.html_file.empty()) session->load_html(automation::read_file(config.html_file, 16 * 1024 * 1024), config.url);
                    else {
                        HttpRequest request; request.url = config.url;
                        request.timeout_ms = config.browser.timeout_ms;
                        request.max_response_bytes = config.browser.max_response_bytes;
                        const auto response = session->request(std::move(request));
                        if (!response.ok()) throw std::runtime_error("watcher request failed");
                        session->load_html(response.body, response.final_url);
                    }
                    const auto data = automation::call(lua, "__pgwatch_sample");
                    if (data.size() > sample_limit) throw std::runtime_error("watcher data exceeds limit");
                    if (data != previous) {
                        automation::send_frame(ipc.get(), {"sample", data});
                        previous = data;
                    } else automation::send_frame(ipc.get(), {"ready"});
                } catch (const std::exception&) { automation::send_frame(ipc.get(), {"error"}); }
                next_sample = automation::Clock::now() + std::chrono::milliseconds(config.interval_ms);
            }
            automation::send_frame(ipc.get(), {"heartbeat"});
            const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(next_sample - automation::Clock::now()).count();
            ::poll(nullptr, 0, static_cast<int>(std::clamp<std::int64_t>(delay, 1, std::min(1000, config.worker_timeout_ms / 3))));
        }
        return 0;
    } catch (const std::exception&) {
        try { automation::send_frame(ipc.get(), {"fatal"}); } catch (...) {}
        return 1;
    }
}
}
