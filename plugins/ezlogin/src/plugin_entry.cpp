#include "prowsetk/ProwseTk-Plugin.h"

#include <array>
#include <string>
#include <string_view>

namespace {
const char* const kCapabilities[] = {
    "authentication", "basic", "bearer", "api-key", "custom-header",
    "redacted-credentials"};

const ProwseTkPluginInfo kInfo = {
    "ezlogin", "0.1.0", "2",
    "Host-mediated login policy for Basic, Bearer, API-key, and custom headers",
    PROWSETK_PLUGIN_NATIVE, "ezlogin", "prowsetk-plugin", kCapabilities, 6};

struct Config {
  std::string mode;
  std::string username;
  std::string password;
  std::string token;
  std::string header;
  std::string value;
  std::string api_key_name = "X-API-Key";
} g_config;

std::string base64(std::string_view input) {
  static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  unsigned value = 0; int bits = -6;
  for (unsigned char c : input) { value = (value << 8) | c; bits += 8; while (bits >= 0) { out.push_back(table[(value >> bits) & 0x3f]); bits -= 6; } }
  if (bits > -6) out.push_back(table[((value << 8) >> (bits + 8)) & 0x3f]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

void clear_config() { g_config = Config{}; }

const char* get(const ProwseTkConfigEntry* entries, size_t count,
                const char* key) {
  for (size_t i = 0; i < count; ++i)
    if (entries[i].key != nullptr && std::string_view(entries[i].key) == key)
      return entries[i].value;
  return nullptr;
}

int plugin_initialize(ProwseTkHost* host) {
  if (!host || !host->api || host->api->abi_version != PROWSETK_PLUGIN_ABI_VERSION)
    return PROWSETK_STATUS_INVALID_ARGUMENT;
  clear_config();
  if (host->api->log) host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                                     "ezlogin plugin initialized");
  return PROWSETK_STATUS_OK;
}

void plugin_shutdown(ProwseTkHost* host) {
  clear_config();
  if (host && host->api && host->api->log)
    host->api->log(host->api->user_data, PROWSETK_LOG_INFO,
                   "ezlogin plugin shutdown");
}

int configure(ProwseTkHost*, const ProwseTkConfigEntry* entries, size_t count) {
  if (!entries && count) return PROWSETK_STATUS_INVALID_ARGUMENT;
  Config next;
  if (const char* v = get(entries, count, "mode")) next.mode = v;
  if (const char* v = get(entries, count, "username")) next.username = v;
  if (const char* v = get(entries, count, "password")) next.password = v;
  if (const char* v = get(entries, count, "token")) next.token = v;
  if (const char* v = get(entries, count, "header")) next.header = v;
  if (const char* v = get(entries, count, "value")) next.value = v;
  if (const char* v = get(entries, count, "api_key_name")) next.api_key_name = v;
  if (next.mode.empty()) next.mode = "none";
  if (next.mode == "basic" && (next.username.empty() || next.password.empty()))
    return PROWSETK_STATUS_INVALID_ARGUMENT;
  if (next.mode == "bearer" && next.token.empty()) return PROWSETK_STATUS_INVALID_ARGUMENT;
  if (next.mode == "api-key" && next.token.empty()) return PROWSETK_STATUS_INVALID_ARGUMENT;
  if (next.mode == "custom-header" && (next.header.empty() || next.value.empty()))
    return PROWSETK_STATUS_INVALID_ARGUMENT;
  if (next.mode != "none" && next.mode != "basic" && next.mode != "bearer" &&
      next.mode != "api-key" && next.mode != "custom-header")
    return PROWSETK_STATUS_INVALID_ARGUMENT;
  g_config = std::move(next);
  return PROWSETK_STATUS_OK;
}

int before_request(ProwseTkHost*, const ProwseTkHttpRequest* request,
                   ProwseTkHookResult* result) {
  if (!request || !result) return PROWSETK_STATUS_INVALID_ARGUMENT;
  result->action = PROWSETK_HOOK_CONTINUE;
  result->message = nullptr;
  result->replacement_request = nullptr;
  if (g_config.mode == "none") return PROWSETK_STATUS_OK;

  static thread_local std::array<ProwseTkHeader, 64> headers;
  static thread_local std::string name;
  static thread_local std::string value;
  if (g_config.mode == "basic") {
    name = "Authorization";
    value = "Basic " + base64(g_config.username + ":" + g_config.password);
  } else if (g_config.mode == "bearer") {
    name = "Authorization";
    value = "Bearer " + g_config.token;
  } else if (g_config.mode == "api-key") {
    name = g_config.api_key_name;
    value = g_config.token;
  } else {
    name = g_config.header;
    value = g_config.value;
  }
  static thread_local ProwseTkHttpRequest replacement;
  replacement = *request;
  size_t copied = request->header_count < headers.size() - 1 ? request->header_count : headers.size() - 1;
  for (size_t i = 0; i < copied; ++i) headers[i] = request->headers[i];
  headers[copied] = {name.c_str(), value.c_str()};
  replacement.headers = headers.data();
  replacement.header_count = copied + 1;
  result->action = PROWSETK_HOOK_REPLACE_REQUEST;
  result->replacement_request = &replacement;
  result->message = "ezlogin authentication header applied";
  return PROWSETK_STATUS_OK;
}

const ProwseTkPlugin kPlugin = {plugin_initialize, plugin_shutdown, []() { return &kInfo; },
                                configure, before_request, nullptr, nullptr};
}

extern "C" PROWSETK_PLUGIN_EXPORT const ProwseTkPlugin* prowsetk_plugin_entry(void) {
  return &kPlugin;
}
