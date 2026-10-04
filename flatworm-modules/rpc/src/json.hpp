#ifndef FLATWORM_RPC_JSON_HPP
#define FLATWORM_RPC_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Flatwork-Module.h"

namespace flatworm::rpc {

inline constexpr std::size_t max_json_depth = 64;
inline constexpr std::size_t max_json_values = 16384;
inline constexpr std::size_t max_batch_size = 128;
inline constexpr std::size_t max_method_bytes = 1024;
inline constexpr std::uint64_t max_safe_integer = 9007199254740991ULL;

// A value-free failure travels only inside the module, never across its C ABI.
struct Failure { FlatwormStatus status; };
[[noreturn]] void invalid();
[[noreturn]] void resource_limit();

// Value-only JSON model. Numbers retain their validated wire spelling, while
// numeric protocol identifiers are compared by value rather than spelling.
struct Json {
    enum class Kind { Null, Boolean, Number, String, Array, Object };
    Json();
    ~Json();
    Json(const Json&);
    Json(Json&&) noexcept;
    Json& operator=(const Json&);
    Json& operator=(Json&&) noexcept;

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> array;
    std::vector<std::pair<std::string, Json>> object;

    static Json boolean_value(bool value);
    static Json number_value(double value);
    static Json parsed_number(std::string_view token);
    static Json string_value(std::string_view value);
    static Json array_value();
    static Json object_value();
    const Json* find(std::string_view key) const;
    void put(std::string key, Json value);
};

void validate_utf8(std::string_view bytes);
Json parse_json(std::string_view source);
std::string encode_json(const Json& value);

}  // namespace flatworm::rpc

#endif
