#ifndef PROWSETK_ERROR_HPP
#define PROWSETK_ERROR_HPP

#include <stdexcept>
#include <string>

namespace prowsetk {

// Stable, layer-agnostic error taxonomy. Every component reports failures with
// one of these codes so callers can react without parsing messages.
enum class ErrorCode {
    Ok = 0,
    InvalidArgument,
    InvalidUrl,
    ParseError,
    NotFound,
    Unsupported,
    NetworkError,
    Timeout,
    TooManyRedirects,
    ResourceLimit,
    SecurityViolation,
    PluginError,
    WasmError,
    LuaError,
    JavaScriptError,
    StorageError,
    IoError,
    Internal
};

const char* to_string(ErrorCode code) noexcept;

// Thrown by the public C++ API on failure. Never allowed to cross the plugin C
// ABI; plugin entry points convert to integer error codes at the boundary.
class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message);

    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

}  // namespace prowsetk

#endif  // PROWSETK_ERROR_HPP
