#pragma once

// Reuse the dependency-free, strict bounded JSON codec used by the RPC module.
// Its value model remains private to the implementation.
#include "../../flatworm-modules/rpc/src/json.hpp"
#include "prowsetk/error.hpp"

namespace prowsetk::protocol {
using Json = ::flatworm::rpc::Json;
inline Json parse(std::string_view text) {
    try { return ::flatworm::rpc::parse_json(text); }
    catch (...) { throw Error(ErrorCode::ParseError, "invalid protocol JSON"); }
}
inline std::string encode(const Json& value) {
    try { return ::flatworm::rpc::encode_json(value); }
    catch (...) { throw Error(ErrorCode::ResourceLimit, "protocol JSON limit"); }
}
inline std::string string(const Json& value, std::string_view key) {
    const auto* field = value.find(key);
    if (!field || field->kind != Json::Kind::String)
        throw Error(ErrorCode::ParseError, "missing protocol string");
    return field->text;
}
inline void put(Json& value, std::string key, std::string_view text) {
    try { value.put(std::move(key), Json::string_value(text)); }
    catch (...) { throw Error(ErrorCode::InvalidArgument, "invalid protocol string"); }
}
}
