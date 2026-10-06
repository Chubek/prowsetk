#include <replxx.hxx>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

#include "prowsetk/lua_runtime.hpp"

namespace {
using prowsetk::LuaArgument;
using prowsetk::LuaRuntime;
constexpr std::size_t line_limit = 65536;
constexpr std::size_t chunk_limit = 262144;
struct Options {
    std::vector<LuaArgument> arguments;
    std::string root, script;
    bool batch = false;
};
void help() {
    std::cout << "Usage: ptk-qute-repl --bridge PRIVATE/bridge.json [options]\n"
        "  --output FILE / --postman FILE  Explicit :export destinations\n"
        "  --require-login                 Require positive DOM evidence before discovery/export\n"
        "  --success-selector CSS / --success-xpath XPATH  Login beacon overrides\n"
        "  --wait-ms N                     Capture/action wait, 1..30000 (default 30000)\n"
        "  --ipc-module FILE               Matching lquteipc module\n"
        "  --root DIR                      Installed/source bridge helper directory\n"
        "  --show-values                   Display target labels in this local console\n"
        "  --include-noise                 Retain telemetry/error-reporting candidates\n"
        "  --batch                         Read commands/Lua from standard input\n"
        "  --script FILE                   Execute a trusted host Lua script and exit\n"
        "Editing, completion, hints and memory-only history use Replxx. :help lists orders.\n";
}
Options parse(int argc, char** argv) {
    Options options;
    bool bridge = false;
    for (int at = 1; at < argc; ++at) {
        const std::string flag = argv[at];
        if (flag == "--batch") { options.batch = true; continue; }
        if (flag == "--require-login" || flag == "--show-values" || flag == "--include-noise") {
            std::string key = flag.substr(2);
            std::replace(key.begin(), key.end(), '-', '_');
            options.arguments.push_back({key, "boolean", "true"});
            continue;
        }
        if (flag != "--bridge" && flag != "--output" && flag != "--postman" && flag != "--success-selector" &&
            flag != "--success-xpath" && flag != "--wait-ms" && flag != "--ipc-module" &&
            flag != "--root" && flag != "--script") throw std::runtime_error("invalid option");
        if (++at >= argc) throw std::runtime_error("missing option value");
        const std::string value = argv[at];
        if (value.empty() || value.size() > 4096) throw std::runtime_error("invalid option value");
        if (flag == "--root") { options.root = value; continue; }
        if (flag == "--script") { options.script = value; continue; }
        if (flag == "--bridge") bridge = true;
        if (flag == "--wait-ms") {
            if (value.find_first_not_of("0123456789") != std::string::npos || value.size() > 5 ||
                std::stoi(value) < 1 || std::stoi(value) > 30000) throw std::runtime_error("invalid wait bound");
        }
        std::string key = flag.substr(2);
        std::replace(key.begin(), key.end(), '-', '_');
        options.arguments.push_back({key, flag == "--wait-ms" ? "integer" : "string", value});
    }
    if (!bridge) throw std::runtime_error("missing bridge descriptor");
    return options;
}
bool has(const Options& options, std::string_view name) {
    return std::any_of(options.arguments.begin(), options.arguments.end(), [&](const auto& arg) { return arg.name == name; });
}
std::filesystem::path executable(const char* arg) {
#ifdef __linux__
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return path;
#endif
    return std::filesystem::absolute(arg);
}
void paths(Options& options, const char* arg) {
    const auto bin = executable(arg);
    // The preset-local module is paired with the chosen executable. Installed
    // paths are relative to bin/ first, so relocatable installations work.
    const auto prefix = bin.parent_path().parent_path();
    if (options.root.empty()) {
        for (const auto& path : {prefix / "share/prowsetk/qutebrowser-bridge", std::filesystem::path(QUTE_INSTALL_SHARE),
                                std::filesystem::path(QUTE_SOURCE_ROOT)}) {
            if (std::filesystem::is_regular_file(path / "lua/console.lua")) { options.root = path.string(); break; }
        }
    }
    if (!has(options, "ipc_module")) {
        for (const auto& path : {bin.parent_path() / "lquteipc.so", prefix / "lib/prowsetk/lua/lquteipc.so",
                                std::filesystem::path(QUTE_INSTALL_IPC), std::filesystem::path(QUTE_IPC_BUILD_PATH)}) {
            if (std::filesystem::is_regular_file(path)) {
                options.arguments.push_back({"ipc_module", "string", path.string()});
                break;
            }
        }
    }
    if (options.root.empty() || !has(options, "ipc_module")) throw std::runtime_error("helpers unavailable");
}
const std::vector<std::string> words = {
    ":capture", ":status", ":targets", ":links", ":forms", ":click", ":fill", ":select", ":check", ":focus",
    ":scroll", ":submit", ":navigate", ":reload", ":discover", ":endpoints", ":export", ":values", ":help", ":quit",
    "qute:capture()", "qute:status()", "qute:targets()", "qute:links()", "qute:forms()", "qute:click(", "qute:fill(",
    "qute:select(", "qute:check(", "qute:scroll(", "qute:submit(", "qute:navigate(", "qute:beacon(", "qute:export()",
    "qute.endpoints", "session:document()", "document:query_selector(", "document:query_selector_all(", "document:xpath(",
    "require('lpdql')", "print(", "local", "function", "for", "if", "then", "end", "true", "false", "nil"
};
void editor(replxx::Replxx& repl) {
    // Expect/PTYs can translate Enter to LF while a Lua/browser operation is
    // running. Accept both forms; multiline Lua uses our continuation prompt.
    repl.bind_key_internal(replxx::Replxx::KEY::control('J'), "commit_line");
    repl.set_max_history_size(200);
    repl.set_unique_history(true);
    repl.set_max_hint_rows(1);
    repl.set_hint_delay(250);
    repl.set_word_break_characters(" \t\n,;=(){}[]\"'");
    repl.set_completion_callback([](const std::string& input, int& length) {
        const auto at = input.find_last_of(" \t\n,;=(){}[]\"'");
        const auto token = input.substr(at == std::string::npos ? 0 : at + 1);
        length = static_cast<int>(token.size());
        replxx::Replxx::completions_t result;
        for (const auto& word : words) if (word.starts_with(token)) result.emplace_back(word);
        return result;
    });
    repl.set_hint_callback([](const std::string& input, int& length, replxx::Replxx::Color& color) {
        replxx::Replxx::hints_t result;
        length = static_cast<int>(input.size());
        color = replxx::Replxx::Color::GRAY;
        if (input.empty()) return result;
        for (const auto& word : words) if (word.starts_with(input) && word != input) result.push_back(word);
        return result;
    });
    repl.set_highlighter_callback([](const std::string& input, replxx::Replxx::colors_t& colors) {
        if (input.starts_with(':')) {
            const auto end = input.find(' ');
            std::fill_n(colors.begin(), std::min(colors.size(), end == std::string::npos ? input.size() : end),
                replxx::Replxx::Color::CYAN);
        }
    });
}
struct Cleanup {
    LuaRuntime& runtime;
    ~Cleanup() { runtime.call("__qute_close"); }
};
}

int main(int argc, char** argv) {
    for (int at = 1; at < argc; ++at) if (std::string_view(argv[at]) == "--help" || std::string_view(argv[at]) == "-h") {
        help(); return 0;
    }
    Options options;
    try { options = parse(argc, argv); paths(options, argv[0]); }
    catch (...) { std::cerr << "ptk-qute-repl: invalid options or unavailable helpers; see --help\n"; return 2; }
    LuaRuntime runtime;
    Cleanup cleanup{runtime};
    if (!runtime.run_file((std::filesystem::path(options.root) / "lua/console.lua").string()).ok ||
        !runtime.call_function("__qute_start", options.arguments).ok) {
        std::cerr << "ptk-qute-repl: bridge initialization failed; check private descriptor, broker and matching IPC module\n";
        return 1;
    }
    if (!options.script.empty()) {
        if (runtime.run_file(options.script).ok) return 0;
        std::cerr << "ptk-qute-repl: trusted Lua script failed; raw error omitted\n"; return 1;
    }
    replxx::Replxx repl;
    editor(repl);
    const bool interactive = !options.batch && ::isatty(STDIN_FILENO);
    std::string pending;
    bool failed = false;
    for (;;) {
        std::string line;
        if (interactive) {
            errno = 0;
            const auto* input = repl.input(pending.empty() ? "qute> " : " ...> ");
            if (!input) {
                if (errno == EAGAIN) { pending.clear(); continue; }
                break;
            }
            line = input;
        } else if (!std::getline(std::cin, line)) break;
        if (line.size() > line_limit || pending.size() + line.size() + 1 > chunk_limit) {
            std::cerr << "qute: input limit exceeded\n"; pending.clear(); failed = true; continue;
        }
        if (line == ":quit") break;
        if (line == ":cancel") { pending.clear(); continue; }
        if (pending.empty() && line.empty()) continue;
        pending += line;
        std::string status;
        const auto result = runtime.call_function("__qute_eval", {{"source", "string", pending}}, &status);
        if (status == "more") { pending += '\n'; continue; }
        if (interactive && result.ok && status == "ok") repl.history_add(pending);
        if (!result.ok || status == "failed") failed = true;
        if (status == "quit") break;
        pending.clear();
    }
    if (!pending.empty()) { std::cerr << "qute: incomplete Lua input discarded\n"; failed = true; }
    repl.history_clear(); // Input may include private values; never save history.
    return !interactive && failed ? 1 : 0;
}
