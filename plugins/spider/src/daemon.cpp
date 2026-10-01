#include "protocol.hpp"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <poll.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>

extern char** environ;
namespace spider = prowsetk::plugins::spider;
namespace {
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t stopping = 0;
void stop_signal(int) { stopping = 1; }
struct Connection {
    spider::Fd fd;
    std::string input, output;
    Clock::time_point touched = Clock::now();
    bool waiting = false;
};
struct Worker {
    spider::Fd fd;
    pid_t pid = -1;
    std::string input, output;
    int client = -1;
    bool newly_created = false;
    Clock::time_point touched = Clock::now();
};
void nonblocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK); }
bool read_available(int fd, std::string& input) {
    char bytes[8192];
    for (;;) {
        const auto n = ::recv(fd, bytes, sizeof(bytes), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (n <= 0) return false;
        input.append(bytes, static_cast<std::size_t>(n));
        if (input.size() > spider::max_message_bytes + 4) return false;
    }
}
bool write_available(int fd, std::string& output) {
    while (!output.empty()) {
        const auto n = ::send(fd, output.data(), output.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (n <= 0) return false;
        output.erase(0, static_cast<std::size_t>(n));
    }
    return true;
}
Worker launch(const std::string& executable, const std::filesystem::path& directory, const std::string& name) {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) throw std::runtime_error("IoError");
    spider::Fd parent(pair[0]), child(pair[1]);
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, child.get(), STDIN_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, parent.get());
    std::vector<std::string> args{executable, "--worker", name, "--directory", directory.string()};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const auto result = ::posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    if (result != 0) throw std::runtime_error("IoError");
    nonblocking(parent.get());
    Worker worker; worker.fd = std::move(parent); worker.pid = pid;
    return worker;
}
void terminate(Worker& worker) {
    if (worker.pid > 0) {
        ::kill(worker.pid, SIGKILL);
        while (::waitpid(worker.pid, nullptr, 0) < 0 && errno == EINTR) {}
        worker.pid = -1;
    }
    worker.fd = spider::Fd{};
}
struct Workers : std::map<std::string, Worker> {
    ~Workers() { for (auto& [name, worker] : *this) { (void)name; terminate(worker); } }
};
int worker_main(const std::string& name, const std::filesystem::path& directory) {
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (::getppid() == 1) return 0;
#ifndef SPIDER_SANITIZED
    const rlimit memory{1024u * 1024u * 1024u, 1024u * 1024u * 1024u};
    ::setrlimit(RLIMIT_AS, &memory);
#endif
    const rlimit cores{0, 0}; ::setrlimit(RLIMIT_CORE, &cores);
    spider::set_deadline(STDIN_FILENO, 5000);
    spider::Engine engine(name, directory / name);
    for (;;) {
        if (!spider::send_frame(STDIN_FILENO, {"heartbeat"})) return 0;
        pollfd fd{STDIN_FILENO, POLLIN, 0};
        const auto ready = ::poll(&fd, 1, 200);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (fd.revents & (POLLHUP | POLLERR | POLLNVAL))) return 0;
        if (ready > 0 && (fd.revents & POLLIN)) {
            const auto command = spider::receive_frame(STDIN_FILENO);
            const auto reply = engine.command(command);
            if (!spider::send_frame(STDIN_FILENO, {reply.ok ? "ok" : "error", reply.json})) return 0;
        } else engine.tick();
    }
}
std::size_t number(std::string_view text, std::size_t maximum) {
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0 || value > maximum)
        throw std::runtime_error("InvalidArgument");
    return value;
}
}

int main(int argc, char** argv) {
    try {
        std::string socket = spider::default_socket(), worker_name;
        auto directory = spider::default_directory();
        std::size_t max_spiders = 16;
        int watchdog_ms = 30000;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout << "ptkspiderd [--socket PATH] [--directory DIR] [--max-spiders N] [--job-timeout-ms N]\n"
                    "Foreground service; SIGTERM/SIGINT shut down workers and preserve state.\n";
                return 0;
            }
            if (++i >= argc) throw std::runtime_error("InvalidArgument");
            if (arg == "--socket") socket = argv[i];
            else if (arg == "--directory") directory = argv[i];
            else if (arg == "--worker") worker_name = argv[i];
            else if (arg == "--max-spiders") max_spiders = number(argv[i], 64);
            else if (arg == "--job-timeout-ms") watchdog_ms = static_cast<int>(number(argv[i], 3600000));
            else throw std::runtime_error("InvalidArgument");
        }
        if (!worker_name.empty()) {
            if (!spider::valid_name(worker_name)) return 2;
            return worker_main(worker_name, directory);
        }
        ::signal(SIGPIPE, SIG_IGN);
        ::signal(SIGTERM, stop_signal); ::signal(SIGINT, stop_signal);
        ::umask(0077);
        // The state-directory lock prevents competing daemons, including those
        // listening on different sockets, from owning the same LMDB workers.
        spider::Cache registry(directory / "registry");
        spider::Fd lock(::open((directory / "daemon.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (lock.get() < 0 || ::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) throw std::runtime_error("AlreadyRunning");
        spider::Fd listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        if (socket.size() >= sizeof(address.sun_path)) throw std::runtime_error("InvalidArgument");
        std::memcpy(address.sun_path, socket.c_str(), socket.size() + 1);
        struct stat existing{};
        if (::lstat(socket.c_str(), &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid()) throw std::runtime_error("SecurityViolation");
            spider::Fd probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
            if (::connect(probe.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
                throw std::runtime_error("AlreadyRunning");
            if (errno != ECONNREFUSED || ::unlink(socket.c_str()) != 0) throw std::runtime_error("IoError");
        }
        if (listener.get() < 0 || ::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::chmod(socket.c_str(), 0600) != 0 || ::listen(listener.get(), 64) != 0) throw std::runtime_error("IoError");
        struct SocketCleanup { std::string path; ~SocketCleanup() { ::unlink(path.c_str()); } } cleanup{socket};
        nonblocking(listener.get());
        const auto executable = std::filesystem::canonical("/proc/self/exe").string();
        Workers workers;
        for (const auto& [name, value] : registry.query("", 64)) {
            (void)value;
            if (!spider::valid_name(name) || workers.size() >= max_spiders) continue;
            workers.emplace(name, launch(executable, directory, name));
        }
        std::map<int, Connection> clients;
        auto reply = [&](int id, const spider::Reply& result) {
            const auto it = clients.find(id);
            if (it == clients.end()) return;
            it->second.output = spider::frame({result.ok ? "ok" : "error", result.json});
            it->second.waiting = false; it->second.touched = Clock::now();
        };
        bool shutdown_requested = false;
        std::cout << "ptkspiderd ready\n" << std::flush;
        while (!stopping && !shutdown_requested) {
            std::vector<pollfd> descriptors{{listener.get(), POLLIN, 0}};
            for (const auto& [fd, client] : clients)
                descriptors.push_back({fd, static_cast<short>((client.waiting ? 0 : POLLIN) | (!client.output.empty() ? POLLOUT : 0)), 0});
            for (const auto& [name, worker] : workers) {
                (void)name;
                if (worker.fd.get() >= 0) descriptors.push_back({worker.fd.get(), static_cast<short>(POLLIN | (!worker.output.empty() ? POLLOUT : 0)), 0});
            }
            ::poll(descriptors.data(), descriptors.size(), 100);
            if (descriptors[0].revents & POLLIN) {
                spider::Fd fd(::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
                if (fd.get() >= 0 && clients.size() < 64) {
                    const int id = fd.get();
                    Connection connection; connection.fd = std::move(fd);
                    clients.emplace(id, std::move(connection));
                }
            }
            std::map<int, short> events;
            for (const auto& descriptor : descriptors) events[descriptor.fd] = descriptor.revents;
            for (auto it = clients.begin(); it != clients.end();) {
                const int id = it->first;
                auto& client = it->second;
                bool alive = !(events[id] & (POLLERR | POLLNVAL));
                if (alive && !client.waiting && client.output.empty() && (events[id] & POLLIN)) {
                    alive = read_available(id, client.input);
                    try {
                        if (auto request = spider::take_frame(client.input)) {
                            client.waiting = true;
                            const auto& args = *request;
                            if (args.size() < 2 || args.size() > 67) reply(id, spider::failure("InvalidArgument"));
                            else {
                                const auto& name = args[0]; const auto& cmd = args[1];
                                if (name.empty() && cmd == "ping" && args.size() == 2) reply(id, {true, "{\"daemon\":\"ptkspiderd\",\"protocol\":1}"});
                                else if (name.empty() && cmd == "list" && args.size() == 2) {
                                    std::string out = "[";
                                    for (const auto& [n, w] : workers) {
                                        if (out.size() > 1) out += ',';
                                        out += "{\"name\":" + spider::json_quote(n) + ",\"alive\":" +
                                            (w.pid > 0 ? "true" : "false") + ",\"busy\":" + (w.client != -1 ? "true" : "false") + "}";
                                    }
                                    reply(id, {true, out + "]"});
                                } else if (name.empty() && cmd == "shutdown" && args.size() == 2) {
                                    // Flush this small acknowledgement before leaving the event loop.
                                    spider::send_frame(id, {"ok", "{}"}); shutdown_requested = true;
                                } else if (!spider::valid_name(name)) reply(id, spider::failure("InvalidArgument"));
                                else {
                                    auto worker = workers.find(name);
                                    if (cmd == "create") {
                                        if (worker != workers.end() || std::filesystem::exists(directory / name))
                                            reply(id, spider::failure("AlreadyExists"));
                                        else if (workers.size() >= max_spiders) reply(id, spider::failure("ResourceLimit"));
                                        else {
                                            registry.write({{name, "1"}});
                                            worker = workers.emplace(name, launch(executable, directory, name)).first;
                                            worker->second.newly_created = true;
                                        }
                                    }
                                    if (client.output.empty()) {
                                        if (worker == workers.end()) reply(id, spider::failure("NotFound"));
                                        else if (cmd == "stop" || cmd == "remove") {
                                            terminate(worker->second);
                                            if (worker->second.client >= 0) reply(worker->second.client, spider::failure("Cancelled"));
                                            worker->second.client = -1;
                                            if (cmd == "remove") {
                                                registry.write({}, {name}); workers.erase(worker);
                                                std::filesystem::remove_all(directory / name);
                                            }
                                            reply(id, {true, "{}"});
                                        } else if (worker->second.client != -1) reply(id, spider::failure("Busy"));
                                        else {
                                            if (worker->second.pid < 0) worker->second = launch(executable, directory, name);
                                            worker->second.client = id; worker->second.touched = Clock::now();
                                            worker->second.output = spider::frame({args.begin() + 1, args.end()});
                                        }
                                    }
                                }
                            }
                        }
                    } catch (...) { reply(id, spider::failure("InvalidArgument")); }
                }
                if (!client.output.empty() && (events[id] & POLLOUT)) {
                    alive = write_available(id, client.output);
                    if (client.output.empty()) alive = false;
                }
                if (!client.waiting && Clock::now() - client.touched > std::chrono::seconds(5)) alive = false;
                if (events[id] & POLLHUP) alive = false;
                if (!alive) {
                    // A disconnected client's operation is still executed once;
                    // leave the worker lease until its response or watchdog.
                    for (auto& [name, worker] : workers) {
                        (void)name;
                        if (worker.client == id) worker.client = -2;
                    }
                    it = clients.erase(it);
                } else ++it;
            }
            for (auto it = workers.begin(); it != workers.end();) {
                auto& worker = it->second;
                if (worker.pid < 0) { ++it; continue; }
                const auto event = events[worker.fd.get()];
                bool alive = !(event & (POLLHUP | POLLERR | POLLNVAL));
                if (alive && (event & POLLOUT)) alive = write_available(worker.fd.get(), worker.output);
                if (alive && (event & POLLIN)) {
                    alive = read_available(worker.fd.get(), worker.input);
                    try {
                        while (auto message = spider::take_frame(worker.input)) {
                            worker.touched = Clock::now();
                            if (message->size() == 1 && (*message)[0] == "heartbeat") continue;
                            if (message->size() != 2) { alive = false; break; }
                            reply(worker.client, {(*message)[0] == "ok", (*message)[1]}); worker.client = -1;
                            if (worker.newly_created && (*message)[0] != "ok") { alive = false; break; }
                            worker.newly_created = false;
                        }
                    } catch (...) { alive = false; }
                }
                const bool timed_out = Clock::now() - worker.touched > std::chrono::milliseconds(watchdog_ms);
                if (!alive || timed_out) {
                    if (worker.client >= 0) reply(worker.client, spider::failure(timed_out ? "Timeout" : "WorkerExited"));
                    worker.client = -1;
                    terminate(worker);
                    if (worker.newly_created) {
                        registry.write({}, {it->first});
                        std::filesystem::remove_all(directory / it->first);
                        it = workers.erase(it); continue;
                    }
                    // Recover paused, preserving the durable frontier/cache. A
                    // failed startup is left stopped rather than crash-looped.
                }
                ++it;
            }
        }
        return 0;
    } catch (...) {
        std::cerr << "ptkspiderd: startup or worker failure\n";
        return 1;
    }
}
