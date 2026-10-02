#include "pagewatch.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <poll.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace prowsetk::pagewatch {
namespace {
namespace pa = automation;
namespace fs = std::filesystem;
volatile std::sig_atomic_t stop_daemon = 0;
void stop(int) { stop_daemon = 1; }
void quiet_child() {
    const int null = ::open("/dev/null", O_RDWR);
    if (null >= 0) { for (int fd = 0; fd < 3; ++fd) ::dup2(null, fd); if (null > 2) ::close(null); }
}
void terminate(pid_t& pid) {
    if (pid <= 0) return;
    ::kill(-pid, SIGKILL);
    ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    pid = 0;
}
std::uint64_t sequence(std::string_view text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw std::runtime_error("invalid sequence");
    return value;
}
struct Update {
    std::uint64_t sequence = 0;
    std::string data;
    bool initial = false;
    std::string json() const {
        return "{\"sequence\":" + std::to_string(sequence) + ",\"initial\":" + (initial ? "true" : "false") + ",\"data\":" + pa::json_quote(data) + "}";
    }
};
struct Watch {
    std::string name, state = "paused", data, buffer;
    WatchConfig config;
    pid_t pid = 0;
    pa::Fd ipc;
    pa::Clock::time_point heartbeat;
    std::uint64_t seq = 0, changes = 0, errors = 0, actions = 0, action_failures = 0, action_dropped = 0;
    std::deque<Update> updates;
    std::size_t history_bytes = 0;
    std::string status() const {
        return "{\"name\":" + pa::json_quote(name) + ",\"state\":" + pa::json_quote(state) +
            ",\"pid\":" + std::to_string(pid) + ",\"sequence\":" + std::to_string(seq) +
            ",\"changes\":" + std::to_string(changes) + ",\"errors\":" + std::to_string(errors) +
            ",\"actions\":" + std::to_string(actions) + ",\"action_failures\":" + std::to_string(action_failures) +
            ",\"action_dropped\":" + std::to_string(action_dropped) + ",\"sample_bytes\":" + std::to_string(data.size()) + "}";
    }
};
struct Client {
    pa::Fd fd;
    std::string input, output;
    std::size_t sent = 0;
    pa::Clock::time_point deadline = pa::Clock::now() + std::chrono::seconds(2);
};
struct Job { std::string name, data; std::uint64_t sequence = 0; bool initial = false; };
struct Action { pid_t pid = 0; std::string name; pa::Clock::time_point deadline; };
class Daemon {
public:
    explicit Daemon(DaemonConfig config) : config_(std::move(config)) {}
    ~Daemon() {
        for (auto& [name, watch] : watches_) { (void)name; terminate(watch.pid); }
        for (auto& action : actions_) terminate(action.pid);
        if (owns_socket_) ::unlink((config_.directory / "pgwatch.sock").c_str());
    }
    int run();
private:
    DaemonConfig config_;
    pa::Fd lock_, listener_;
    bool owns_socket_ = false, shutdown_ = false;
    std::map<std::string, Watch> watches_;
    std::map<int, Client> clients_;
    std::deque<Job> jobs_;
    std::vector<Action> actions_;
    std::string action_source_;
    fs::path directory(const Watch& watch) const { return config_.directory / "watchers" / watch.name; }
    void save(const Watch& watch) const;
    void start(Watch& watch);
    void halt(Watch& watch, std::string state = "paused");
    std::string command(const std::vector<std::string>& fields);
    void message(Watch& watch, const std::vector<std::string>& fields);
    void actions();
};
void Daemon::save(const Watch& watch) const {
    std::string state = "sequence = " + std::to_string(watch.seq) + "\nchanges = " + std::to_string(watch.changes) +
        "\nerrors = " + std::to_string(watch.errors) + "\nactions = " + std::to_string(watch.actions) +
        "\naction_failures = " + std::to_string(watch.action_failures) + "\naction_dropped = " + std::to_string(watch.action_dropped) + "\n";
    pa::write_file(directory(watch) / "state.toml", state);
}
void Daemon::start(Watch& watch) {
    if (watch.pid > 0) return;
    std::array<int, 2> pair{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) != 0) throw std::runtime_error("cannot create worker IPC");
    pa::Fd parent(pair[0]), child(pair[1]);
    const auto path = directory(watch).string();
    const pid_t pid = ::fork();
    if (pid < 0) throw std::runtime_error("cannot start worker");
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(child.get(), 3);
        ::fcntl(3, F_SETFD, 0);
        ::close_range(4, ~0u, 0);
        quiet_child();
        ::execl("/proc/self/exe", "pgwatchd", "--worker", path.c_str(), "--ipc-fd", "3", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::fcntl(parent.get(), F_SETFL, ::fcntl(parent.get(), F_GETFL, 0) | O_NONBLOCK);
    watch.pid = pid; watch.ipc = std::move(parent);
    watch.buffer.clear(); watch.state = "starting"; watch.heartbeat = pa::Clock::now();
}
void Daemon::halt(Watch& watch, std::string state) {
    terminate(watch.pid); watch.ipc = pa::Fd(); watch.buffer.clear(); watch.state = std::move(state);
}
void Daemon::message(Watch& watch, const std::vector<std::string>& fields) {
    if (fields.empty()) throw std::runtime_error("empty worker message");
    watch.heartbeat = pa::Clock::now();
    if (fields[0] == "sample" && fields.size() == 2 && fields[1].size() <= sample_limit) {
        if (watch.data == fields[1]) { watch.state = "running"; return; }
        const bool initial = watch.seq == 0;
        watch.data = fields[1]; ++watch.seq;
        if (!initial) ++watch.changes;
        watch.state = "running";
        pa::write_file(directory(watch) / "tab.json", watch.data);
        watch.updates.push_back({watch.seq, watch.data, initial});
        watch.history_bytes += watch.data.size();
        while (watch.updates.size() > 16 || watch.history_bytes > 512 * 1024) {
            watch.history_bytes -= watch.updates.front().data.size(); watch.updates.pop_front();
        }
        if (!action_source_.empty() && (!initial || config_.emit_initial)) {
            if (jobs_.size() < 32) jobs_.push_back({watch.name, watch.data, watch.seq, initial});
            else ++watch.action_dropped;
        }
        save(watch);
    } else if (fields[0] == "ready" && fields.size() == 1) watch.state = "running";
    else if (fields[0] == "error" && fields.size() == 1) { watch.state = "error"; ++watch.errors; save(watch); }
    else if (fields[0] == "fatal" && fields.size() == 1) { ++watch.errors; halt(watch, "failed"); save(watch); }
    else if (fields[0] != "heartbeat" || fields.size() != 1) throw std::runtime_error("invalid worker message");
}
std::string Daemon::command(const std::vector<std::string>& fields) {
    if (fields.empty()) throw std::runtime_error("empty command");
    const auto& cmd = fields[0];
    if (fields.size() == 1) {
        if (cmd == "ping") return "{\"ready\":true}";
        if (cmd == "shutdown") { shutdown_ = true; return "{\"stopping\":true}"; }
        if (cmd == "list") {
            std::string result = "[";
            for (const auto& [name, watch] : watches_) { (void)name; if (result.size() > 1) result += ','; result += watch.status(); }
            return result + ']';
        }
    }
    if (fields.size() < 2 || !pa::valid_name(fields[1])) throw std::runtime_error("invalid watcher name");
    auto it = watches_.find(fields[1]);
    if ((cmd == "deploy" || cmd == "update") && fields.size() == 4) {
        if (fields[2].size() > script_limit || fields[3].size() > script_limit || fields[2].empty()) throw std::runtime_error("deployment exceeds limit");
        const auto config = parse_watch_config(fields[3]);
        if ((cmd == "deploy" && it != watches_.end()) || (cmd == "update" && it == watches_.end())) throw std::runtime_error("deployment state conflict");
        if (it == watches_.end()) {
            if (watches_.size() >= 32) throw std::runtime_error("watcher limit reached");
            Watch watch; watch.name = fields[1]; watch.config = config;
            pa::private_directory(directory(watch));
            it = watches_.emplace(watch.name, std::move(watch)).first;
        } else halt(it->second);
        auto& watch = it->second;
        pa::write_file(directory(watch) / "watcher.lua", fields[2]);
        pa::write_file(directory(watch) / "Watcher.toml", fields[3]);
        watch.config = config; save(watch); start(watch);
        return watch.status();
    }
    if (it == watches_.end()) throw std::runtime_error("watcher not found");
    auto& watch = it->second;
    if (fields.size() == 2) {
        if (cmd == "status") return watch.status();
        if (cmd == "data") return "{\"name\":" + pa::json_quote(watch.name) + ",\"sequence\":" + std::to_string(watch.seq) + ",\"data\":" + pa::json_quote(watch.data) + "}";
        if (cmd == "pause") { halt(watch); return watch.status(); }
        if (cmd == "resume") { start(watch); return watch.status(); }
        if (cmd == "check") { if (watch.pid <= 0) throw std::runtime_error("watcher is paused"); ::kill(watch.pid, SIGUSR1); return watch.status(); }
        if (cmd == "remove") {
            halt(watch);
            std::erase_if(jobs_, [&](const auto& job) { return job.name == watch.name; });
            for (auto& action : actions_) if (action.name == watch.name) terminate(action.pid);
            fs::remove_all(directory(watch)); watches_.erase(it);
            return "{\"removed\":true}";
        }
    }
    if (cmd == "updates" && fields.size() == 3) {
        const auto since = sequence(fields[2]);
        const bool lost = since < watch.seq && (watch.updates.empty() || since < watch.updates.front().sequence - 1);
        std::string events = "[";
        std::uint64_t last = since;
        bool more = false;
        for (const auto& event : watch.updates) {
            if (event.sequence <= since) continue;
            const auto encoded = event.json();
            if (events.size() + encoded.size() > 900 * 1024) { more = true; break; }
            if (events.size() > 1) events += ',';
            events += encoded; last = event.sequence;
        }
        return "{\"updates\":" + events + "],\"last_sequence\":" + std::to_string(last) + ",\"history_lost\":" +
            (lost ? "true" : "false") + ",\"more\":" + (more ? "true" : "false") + "}";
    }
    throw std::runtime_error("invalid command");
}
void Daemon::actions() {
    for (auto it = actions_.begin(); it != actions_.end();) {
        int status = 0;
        const auto done = it->pid > 0 ? ::waitpid(it->pid, &status, WNOHANG) : -1;
        if (done == 0 && pa::Clock::now() < it->deadline) { ++it; continue; }
        const bool ok = done > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done > 0) it->pid = 0;
        terminate(it->pid);
        if (const auto watch = watches_.find(it->name); watch != watches_.end()) {
            if (!ok) ++watch->second.action_failures;
            save(watch->second);
        }
        it = actions_.erase(it);
    }
    while (actions_.size() < 4 && !jobs_.empty()) {
        const auto eligible = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto& job) {
            return std::none_of(actions_.begin(), actions_.end(), [&](const auto& action) { return action.name == job.name; });
        });
        if (eligible == jobs_.end()) break;
        const auto job = *eligible; jobs_.erase(eligible);
        const auto watch = watches_.find(job.name);
        if (watch == watches_.end()) continue;
        const auto working = directory(watch->second);
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::setpgid(0, 0); ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            ::close_range(3, ~0u, 0); quiet_child();
            if (::getppid() == 1 || ::chdir(working.c_str()) != 0) ::_exit(1);
            int code = 1;
            try {
                Browser browser;
                LuaRuntime lua; lua.bind_browser(&browser);
                const std::vector<LuaArgument> arguments{
                    {"name", "string", job.name}, {"data", "string", job.data},
                    {"sequence", "integer", std::to_string(job.sequence)}, {"initial", "boolean", job.initial ? "true" : "false"}};
                if (lua.run(action_source_, "pagewatch-action").ok && pa::call(lua, "main", arguments) == "0") code = 0;
            } catch (...) {}
            ::_exit(code);
        }
        if (pid < 0) { ++watch->second.action_failures; save(watch->second); continue; }
        ::setpgid(pid, pid);
        ++watch->second.actions; save(watch->second);
        actions_.push_back({pid, job.name, pa::Clock::now() + std::chrono::milliseconds(config_.action_timeout_ms)});
    }
}
int Daemon::run() {
    ::umask(0077);
    pa::private_directory(config_.directory);
    pa::private_directory(config_.directory / "watchers");
    lock_ = pa::Fd(::open((config_.directory / ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock_.get() < 0 || ::flock(lock_.get(), LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("daemon already running");
    if (!config_.action_script.empty()) action_source_ = pa::read_file(config_.action_script, script_limit);
    for (const auto& entry : fs::directory_iterator(config_.directory / "watchers")) {
        const auto name = entry.path().filename().string();
        if (!pa::valid_name(name) || watches_.size() >= 32) throw std::runtime_error("invalid saved deployment");
        pa::private_directory(entry.path());
        Watch watch; watch.name = name;
        watch.config = parse_watch_config(pa::read_file(entry.path() / "Watcher.toml", script_limit));
        if (fs::exists(entry.path() / "tab.json")) watch.data = pa::read_file(entry.path() / "tab.json", sample_limit);
        if (fs::exists(entry.path() / "state.toml")) {
            const auto state = toml::parse(pa::read_file(entry.path() / "state.toml"));
            auto field = [&](std::string_view key) { const auto value = state[key].value<std::int64_t>(); if (!value || *value < 0) throw std::runtime_error("invalid saved state"); return static_cast<std::uint64_t>(*value); };
            watch.seq = field("sequence"); watch.changes = field("changes"); watch.errors = field("errors");
            watch.actions = field("actions"); watch.action_failures = field("action_failures"); watch.action_dropped = field("action_dropped");
        }
        watches_.emplace(name, std::move(watch));
    }
    const auto socket = config_.directory / "pgwatch.sock";
    struct stat status{};
    if (::lstat(socket.c_str(), &status) == 0) {
        if (!S_ISSOCK(status.st_mode) || status.st_uid != ::geteuid()) throw std::runtime_error("unsafe socket path");
        ::unlink(socket.c_str());
    }
    listener_ = pa::Fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    const auto path = socket.string();
    if (listener_.get() < 0 || path.size() >= sizeof(address.sun_path)) throw std::runtime_error("invalid socket path");
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) throw std::runtime_error("socket bind failed");
    owns_socket_ = true;
    if (::chmod(socket.c_str(), 0600) != 0 || ::listen(listener_.get(), 32) != 0) throw std::runtime_error("socket listen failed");
    std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
    // Workers inherit this until their own handler is installed, so an early
    // `check` cannot kill a still-initializing process with default SIGUSR1.
    std::signal(SIGUSR1, SIG_IGN);
    std::cout << "pgwatchd ready\n" << std::flush;
    while (!stop_daemon) {
        if (shutdown_ && clients_.empty()) break;
        actions();
        std::vector<pollfd> pollers{{listener_.get(), POLLIN, 0}};
        std::vector<std::pair<std::string, pid_t>> workers;
        for (const auto& [name, watch] : watches_) if (watch.pid > 0) {
            workers.emplace_back(name, watch.pid); pollers.push_back({watch.ipc.get(), POLLIN, 0});
        }
        std::vector<int> clients;
        for (const auto& [fd, client] : clients_) { clients.push_back(fd); pollers.push_back({fd, static_cast<short>(client.output.empty() ? POLLIN : POLLOUT), 0}); }
        const auto ready = ::poll(pollers.data(), static_cast<nfds_t>(pollers.size()), 25);
        if (ready < 0 && errno != EINTR) throw std::runtime_error("poll failed");
        if (!shutdown_ && (pollers[0].revents & POLLIN)) {
            for (int accepted = 0; accepted < 16; ++accepted) {
                pa::Fd fd(::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
                if (fd.get() < 0) break;
                ucred peer{}; socklen_t length = sizeof(peer);
                if (clients_.size() >= 64 || ::getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 || peer.uid != ::geteuid()) continue;
                const int id = fd.get(); Client client; client.fd = std::move(fd); clients_.emplace(id, std::move(client));
            }
        }
        for (std::size_t i = 0; i < workers.size(); ++i) {
            auto found = watches_.find(workers[i].first);
            if (found == watches_.end() || found->second.pid != workers[i].second) continue;
            auto& watch = found->second;
            const auto events = pollers[i + 1].revents;
            if (events & (POLLIN | POLLHUP | POLLERR)) {
                try {
                    for (std::size_t received = 0; received < pa::message_limit;) {
                        char data[4096];
                        const auto count = ::recv(watch.ipc.get(), data, sizeof(data), 0);
                        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                        if (count < 0 && errno == EINTR) continue;
                        if (count <= 0) throw std::runtime_error("worker disconnected");
                        received += static_cast<std::size_t>(count); watch.buffer.append(data, static_cast<std::size_t>(count));
                        while (auto fields = pa::take_frame(watch.buffer)) { message(watch, *fields); if (!watch.pid) break; }
                        if (!watch.pid) break;
                    }
                } catch (...) { ++watch.errors; halt(watch, "failed"); save(watch); }
            }
            if (watch.pid > 0 && pa::Clock::now() - watch.heartbeat > std::chrono::milliseconds(watch.config.worker_timeout_ms)) {
                ++watch.errors; halt(watch, "timed_out"); save(watch);
            }
        }
        for (std::size_t i = 0; i < clients.size(); ++i) {
            const auto found = clients_.find(clients[i]);
            if (found == clients_.end()) continue;
            auto& client = found->second;
            const auto events = pollers[workers.size() + i + 1].revents;
            bool remove = pa::Clock::now() >= client.deadline;
            if (!remove && client.output.empty() && (events & (POLLIN | POLLHUP | POLLERR))) {
                try {
                    char data[4096];
                    const auto count = ::recv(client.fd.get(), data, sizeof(data), 0);
                    if (count > 0) {
                        client.input.append(data, static_cast<std::size_t>(count));
                        if (auto fields = pa::take_frame(client.input)) {
                            if (!client.input.empty()) throw std::runtime_error("one command per connection");
                            client.output = pa::frame({"ok", command(*fields)});
                        }
                    } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) remove = true;
                } catch (...) { client.output = pa::frame({"error", "{\"error\":\"invalid_command_or_deployment\"}"}); }
            }
            if (!remove && !client.output.empty() && (events & (POLLOUT | POLLIN))) {
                const auto count = ::send(client.fd.get(), client.output.data() + client.sent, client.output.size() - client.sent, MSG_NOSIGNAL);
                if (count > 0) { client.sent += static_cast<std::size_t>(count); remove = client.sent == client.output.size(); }
                else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) remove = true;
            }
            if (remove) clients_.erase(found);
        }
    }
    return 0;
}
}
int daemon(const DaemonConfig& config) { Daemon instance(config); return instance.run(); }
}
