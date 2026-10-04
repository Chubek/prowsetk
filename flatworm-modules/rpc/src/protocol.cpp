#include "protocol.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace flatworm::rpc {
namespace {

bool safe_integer(const Json& value) {
    if (value.kind != Json::Kind::Number || !std::isfinite(value.number) ||
        std::trunc(value.number) != value.number ||
        std::abs(value.number) > static_cast<double>(max_safe_integer)) return false;

    // A fractional wire ID can round to an integer in binary64. Check the
    // validated decimal spelling as well, before using doubles for correlation.
    const std::string_view token(value.text);
    const auto exponent_at = token.find_first_of("eE");
    const auto mantissa = token.substr(0, exponent_at);
    const auto dot = mantissa.find('.');
    const auto fraction = dot == std::string_view::npos ? 0 :
        static_cast<std::int64_t>(mantissa.size() - dot - 1);
    std::int64_t exponent = 0;
    if (exponent_at != std::string_view::npos) {
        auto offset = exponent_at + 1;
        const bool negative = token[offset] == '-';
        if (negative || token[offset] == '+') ++offset;
        constexpr auto cap = static_cast<std::int64_t>(FLATWORM_MODULE_MAX_VALUE_BYTES) + 32;
        for (; offset < token.size(); ++offset) {
            exponent = std::min(cap, exponent * 10 + (token[offset] - '0'));
        }
        if (negative) exponent = -exponent;
    }
    std::int64_t trailing_zeros = 0;
    for (auto offset = mantissa.size(); offset > 0;) {
        const auto digit = mantissa[--offset];
        if (digit == '.' || digit == '-') continue;
        if (digit != '0') return fraction - exponent <= trailing_zeros;
        ++trailing_zeros;
    }
    return true; // All-zero mantissas are integral regardless of their exponent.
}

void method_name(std::string_view method) {
    if (method.size() > max_method_bytes) resource_limit();
    if (method.starts_with("rpc.")) invalid();
    validate_utf8(method);
}

void params_value(const Json& params) {
    if (params.kind != Json::Kind::Object && params.kind != Json::Kind::Array) invalid();
}

void version(const Json& value) {
    const auto* member = value.find("jsonrpc");
    if (member == nullptr || member->kind != Json::Kind::String || member->text != "2.0") invalid();
}

Json envelope(const Json& id) {
    validate_id(id);
    auto value = Json::object_value();
    value.put("jsonrpc", Json::string_value("2.0"));
    value.put("id", id);
    return value;
}

std::string id_key(const Json& id) {
    validate_id(id);
    if (id.kind == Json::Kind::Null) return "null";
    if (id.kind == Json::Kind::String) return "s:" + id.text;
    return "n:" + Json::number_value(id.number == 0 ? 0 : id.number).text;
}

template<class Visitor>
void each_message(const Json& value, Visitor visit) {
    if (value.kind != Json::Kind::Array) { visit(value); return; }
    if (value.array.empty()) invalid();
    if (value.array.size() > max_batch_size) resource_limit();
    for (const auto& item : value.array) visit(item);
}

}  // namespace

void validate_id(const Json& id) {
    if (id.kind != Json::Kind::Null && id.kind != Json::Kind::String && !safe_integer(id)) invalid();
}

void validate_request(const Json& value) {
    version(value);
    const auto* method = value.find("method");
    if (method == nullptr || method->kind != Json::Kind::String ||
        value.find("result") != nullptr || value.find("error") != nullptr) invalid();
    method_name(method->text);
    if (const auto* params = value.find("params")) params_value(*params);
    if (const auto* id = value.find("id")) validate_id(*id);
}

void validate_requests(const Json& value) { each_message(value, validate_request); }

void validate_response(const Json& value) {
    version(value);
    const auto* id = value.find("id");
    const auto* succeeded = value.find("result");
    const auto* failed = value.find("error");
    if (id == nullptr || (succeeded == nullptr) == (failed == nullptr) ||
        value.find("method") != nullptr || value.find("params") != nullptr) invalid();
    validate_id(*id);
    if (failed != nullptr) {
        const auto* code = failed->find("code");
        const auto* message = failed->find("message");
        if (code == nullptr || !safe_integer(*code) || message == nullptr ||
            message->kind != Json::Kind::String) invalid();
    }
}

void validate_responses(const Json& value) { each_message(value, validate_response); }

bool same_id(const Json& left, const Json& right) { return id_key(left) == id_key(right); }

Json request(std::string_view method, const Json* params, const Json* id) {
    method_name(method);
    if (params != nullptr) params_value(*params);
    if (id != nullptr) validate_id(*id);
    auto value = Json::object_value();
    value.put("jsonrpc", Json::string_value("2.0"));
    value.put("method", Json::string_value(method));
    if (params != nullptr) value.put("params", *params);
    if (id != nullptr) value.put("id", *id);
    return value;
}

Json result(const Json& id, Json value) {
    auto reply = envelope(id);
    reply.put("result", std::move(value));
    return reply;
}

Json error(const Json& id, const Json& code, std::string_view message, const Json* data) {
    if (!safe_integer(code)) invalid();
    auto reply = envelope(id);
    auto detail = Json::object_value();
    detail.put("code", code);
    detail.put("message", Json::string_value(message));
    if (data != nullptr) detail.put("data", *data);
    reply.put("error", std::move(detail));
    return reply;
}

Json batch(Json requests) {
    if (requests.kind != Json::Kind::Array) invalid();
    validate_requests(requests);
    std::set<std::string> ids;
    for (const auto& item : requests.array) {
        if (const auto* id = item.find("id")) if (!ids.insert(id_key(*id)).second) invalid();
    }
    return requests;
}

Json correlate(const Json& requests, const Json* responses) {
    validate_requests(requests);
    std::vector<std::string> order;
    std::set<std::string> expected;
    each_message(requests, [&](const Json& item) {
        if (const auto* id = item.find("id")) {
            auto key = id_key(*id);
            if (!expected.insert(key).second) invalid();
            order.push_back(std::move(key));
        }
    });
    auto ordered = Json::array_value();
    if (responses == nullptr) {
        if (!order.empty()) invalid();
        return ordered;
    }
    if (order.empty() || (requests.kind == Json::Kind::Array) !=
        (responses->kind == Json::Kind::Array)) invalid();
    validate_responses(*responses);
    std::map<std::string, const Json*> received;
    each_message(*responses, [&](const Json& item) {
        auto key = id_key(*item.find("id"));
        if (!expected.contains(key) || !received.emplace(key, &item).second) invalid();
    });
    if (received.size() != order.size()) invalid();
    for (const auto& key : order) ordered.array.push_back(*received.at(key));
    return ordered;
}

}  // namespace flatworm::rpc
