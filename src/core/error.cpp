#include "prowsetk/error.hpp"

namespace prowsetk {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::InvalidArgument: return "invalid_argument";
        case ErrorCode::InvalidUrl: return "invalid_url";
        case ErrorCode::ParseError: return "parse_error";
        case ErrorCode::NotFound: return "not_found";
        case ErrorCode::Unsupported: return "unsupported";
        case ErrorCode::NetworkError: return "network_error";
        case ErrorCode::Timeout: return "timeout";
        case ErrorCode::TooManyRedirects: return "too_many_redirects";
        case ErrorCode::ResourceLimit: return "resource_limit";
        case ErrorCode::SecurityViolation: return "security_violation";
        case ErrorCode::PluginError: return "plugin_error";
        case ErrorCode::WasmError: return "wasm_error";
        case ErrorCode::LuaError: return "lua_error";
        case ErrorCode::JavaScriptError: return "javascript_error";
        case ErrorCode::StorageError: return "storage_error";
        case ErrorCode::IoError: return "io_error";
        case ErrorCode::Internal: return "internal";
    }
    return "unknown";
}

Error::Error(ErrorCode code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

}  // namespace prowsetk
