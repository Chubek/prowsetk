#include "json.hpp"

#include <charconv>
#include <cmath>
#include <system_error>

namespace flatworm::rpc {

[[noreturn]] void invalid() { throw Failure{FLATWORM_STATUS_INVALID_ARGUMENT}; }
[[noreturn]] void resource_limit() { throw Failure{FLATWORM_STATUS_RESOURCE_LIMIT}; }

// Instantiate recursive pair/vector operations after Json is complete.
Json::Json() = default;
Json::~Json() = default;
Json::Json(const Json&) = default;
Json::Json(Json&&) noexcept = default;
Json& Json::operator=(const Json&) = default;
Json& Json::operator=(Json&&) noexcept = default;

Json Json::boolean_value(bool value) {
    Json result;
    result.kind = Kind::Boolean;
    result.boolean = value;
    return result;
}

Json Json::number_value(double value) {
    if (!std::isfinite(value)) invalid();
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (converted.ec != std::errc{}) invalid();
    Json result;
    result.kind = Kind::Number;
    result.number = value;
    result.text.assign(buffer, converted.ptr);
    return result;
}

Json Json::parsed_number(std::string_view token) {
    Json result;
    result.kind = Kind::Number;
    const auto converted = std::from_chars(token.data(), token.data() + token.size(), result.number);
    if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size() ||
        !std::isfinite(result.number)) invalid();
    result.text = token;
    return result;
}

Json Json::string_value(std::string_view value) {
    validate_utf8(value);
    Json result;
    result.kind = Kind::String;
    result.text = value;
    return result;
}

Json Json::array_value() { Json result; result.kind = Kind::Array; return result; }
Json Json::object_value() { Json result; result.kind = Kind::Object; return result; }

const Json* Json::find(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (const auto& [name, value] : object) if (name == key) return &value;
    return nullptr;
}

void Json::put(std::string key, Json value) {
    object.emplace_back(std::move(key), std::move(value));
}

}  // namespace flatworm::rpc
