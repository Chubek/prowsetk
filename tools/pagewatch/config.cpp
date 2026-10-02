#include "pagewatch.hpp"
#include <prowsetk/pdql.hpp>

namespace prowsetk::pagewatch {
WatchConfig parse_watch_config(std::string_view source) {
    const auto config = toml::parse(source);
    automation::check_keys(config, {"watcher", "engine", "arguments"});
    const auto* watch = config["watcher"].as_table();
    if (!watch) throw std::runtime_error("missing [watcher] table");
    automation::check_keys(*watch, {"url", "query", "html_file", "interval_ms", "worker_timeout_ms"});
    WatchConfig result;
    result.url = automation::string_setting(*watch, "url");
    automation::origin(result.url);
    result.query = automation::string_setting(*watch, "query", "select tag, text from <h*>");
    pdql::Query query(result.query);
    result.html_file = automation::string_setting(*watch, "html_file");
    result.interval_ms = automation::int_setting(*watch, "interval_ms", 30000, 20, 86400000);
    result.worker_timeout_ms = automation::int_setting(*watch, "worker_timeout_ms", 60000, 100, 300000);
    const auto* engine = config["engine"].as_table();
    result.browser = automation::browser_config(engine ? *engine : toml::table{}, !result.html_file.empty());
    // Heartbeats are emitted at least once per second even between samples.
    if (result.worker_timeout_ms < result.browser.timeout_ms && result.html_file.empty())
        throw std::runtime_error("worker timeout must cover the request timeout");
    if (const auto* arguments = config["arguments"].as_table()) result.arguments = automation::scalar_arguments(*arguments);
    for (const auto& argument : result.arguments) if (argument.name == "url" || argument.name == "query")
        throw std::runtime_error("reserved watcher argument");
    result.arguments.push_back({"url", "url", result.url});
    result.arguments.push_back({"query", "string", result.query});
    return result;
}
}
