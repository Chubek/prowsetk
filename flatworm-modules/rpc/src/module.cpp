#include "module.hpp"
#include "protocol.hpp"

#include <memory>
#include <new>
#include <optional>
#include <span>

namespace flatworm::rpc {
namespace {

struct Instance { std::uint64_t next_id = 1; };
struct Output { Json value; bool advance_id = false; };
using Arguments = std::span<const FlatwormValue>;
using Operation = Output (*)(Arguments, Instance&);

bool host_api(const FlatwormModuleHostApi* api) {
    return api != nullptr && api->abi_version == FLATWORM_MODULE_ABI_VERSION &&
        api->struct_size >= sizeof(FlatwormModuleHostApi) && api->set_result != nullptr;
}

FlatwormStatus initialize(const FlatwormModuleHostApi* api, void** instance) noexcept {
    if (instance == nullptr) return FLATWORM_STATUS_INVALID_ARGUMENT;
    *instance = nullptr;
    if (!host_api(api)) return FLATWORM_STATUS_INVALID_ARGUMENT;
    try {
        auto owned = std::make_unique<Instance>();
        *instance = owned.release();
        return FLATWORM_STATUS_OK;
    } catch (const std::bad_alloc&) { return FLATWORM_STATUS_RESOURCE_LIMIT; }
    catch (...) { return FLATWORM_STATUS_ERROR; }
}

void shutdown(void* instance) noexcept {
    const std::unique_ptr<Instance> owned(static_cast<Instance*>(instance));
}

std::string_view bytes(const FlatwormValue& value) {
    return {value.bytes.data != nullptr ? value.bytes.data : "", value.bytes.size};
}

std::string_view string_argument(const FlatwormValue& value) {
    if (value.type != FLATWORM_VALUE_STRING) invalid();
    return bytes(value);
}

Json json_argument(const FlatwormValue& value) {
    switch (value.type) {
        case FLATWORM_VALUE_NULL: return {};
        case FLATWORM_VALUE_BOOLEAN: return Json::boolean_value(value.boolean != 0);
        case FLATWORM_VALUE_NUMBER: return Json::number_value(value.number);
        case FLATWORM_VALUE_STRING: return Json::string_value(bytes(value));
        case FLATWORM_VALUE_JSON: return parse_json(bytes(value));
        default: invalid();
    }
}

Json wire_argument(const FlatwormValue& value) {
    return value.type == FLATWORM_VALUE_STRING ? parse_json(bytes(value)) : json_argument(value);
}

std::optional<Json> optional_json(Arguments args, std::size_t index) {
    if (index >= args.size() || args[index].type == FLATWORM_VALUE_UNDEFINED) return std::nullopt;
    return json_argument(args[index]);
}

FlatwormStatus invoke(FlatwormCall* call, std::size_t minimum, std::size_t maximum,
                     Operation operation) noexcept {
    try {
        if (call == nullptr || !host_api(call->api) || call->instance == nullptr ||
            call->argument_count < minimum || call->argument_count > maximum ||
            (call->argument_count != 0 && call->arguments == nullptr)) invalid();
        const Arguments args(call->arguments, call->argument_count);
        std::size_t total = 0;
        for (const auto& arg : args) {
            if (arg.type > FLATWORM_VALUE_JSON) invalid();
            if (arg.type != FLATWORM_VALUE_STRING && arg.type != FLATWORM_VALUE_JSON) continue;
            if (arg.bytes.size > FLATWORM_MODULE_MAX_VALUE_BYTES - total) resource_limit();
            if (arg.bytes.data == nullptr && arg.bytes.size != 0) invalid();
            total += arg.bytes.size;
        }
        auto& instance = *static_cast<Instance*>(call->instance);
        auto output = operation(args, instance);
        const auto encoded = encode_json(output.value);
        const FlatwormValue result_value{FLATWORM_VALUE_JSON, 0, 0, {encoded.data(), encoded.size()}};
        const auto status = call->api->set_result(call, &result_value);
        if (status == FLATWORM_STATUS_OK && output.advance_id) ++instance.next_id;
        return status;
    } catch (const Failure& failure) { return failure.status; }
    catch (const std::bad_alloc&) { return FLATWORM_STATUS_RESOURCE_LIMIT; }
    catch (...) { return FLATWORM_STATUS_ERROR; }
}

Output request_operation(Arguments args, Instance& instance) {
    const auto params = optional_json(args, 1);
    auto id = optional_json(args, 2);
    const bool automatic = !id.has_value();
    if (automatic) {
        if (instance.next_id > max_safe_integer) resource_limit();
        id = Json::number_value(static_cast<double>(instance.next_id));
    }
    return {request(string_argument(args[0]), params ? &*params : nullptr, &*id), automatic};
}
Output notification_operation(Arguments args, Instance&) {
    const auto params = optional_json(args, 1);
    return {request(string_argument(args[0]), params ? &*params : nullptr, nullptr)};
}
Output batch_operation(Arguments args, Instance&) { return {batch(wire_argument(args[0]))}; }
Output result_operation(Arguments args, Instance&) {
    return {result(json_argument(args[0]), json_argument(args[1]))};
}
Output error_operation(Arguments args, Instance&) {
    const auto data = optional_json(args, 3);
    return {error(json_argument(args[0]), json_argument(args[1]), string_argument(args[2]),
                  data ? &*data : nullptr)};
}
Output parse_request_operation(Arguments args, Instance&) {
    auto value = wire_argument(args[0]);
    validate_requests(value);
    return {std::move(value)};
}
Output parse_response_operation(Arguments args, Instance&) {
    auto value = wire_argument(args[0]);
    validate_responses(value);
    if (const auto expected = optional_json(args, 1)) {
        const auto* id = value.find("id");
        if (id == nullptr || !same_id(*id, *expected)) invalid();
    }
    return {std::move(value)};
}
Output correlate_operation(Arguments args, Instance&) {
    const auto requests = wire_argument(args[0]);
    std::optional<Json> responses;
    if (args.size() == 2 && args[1].type != FLATWORM_VALUE_UNDEFINED &&
        args[1].type != FLATWORM_VALUE_NULL) {
        if (args[1].type != FLATWORM_VALUE_STRING ||
            string_argument(args[1]).find_first_not_of(" \t\r\n") != std::string_view::npos) {
            responses = wire_argument(args[1]);
        }
    }
    return {correlate(requests, responses ? &*responses : nullptr)};
}

FlatwormStatus request_callback(FlatwormCall* call) noexcept { return invoke(call, 1, 3, request_operation); }
FlatwormStatus notification_callback(FlatwormCall* call) noexcept { return invoke(call, 1, 2, notification_operation); }
FlatwormStatus batch_callback(FlatwormCall* call) noexcept { return invoke(call, 1, 1, batch_operation); }
FlatwormStatus result_callback(FlatwormCall* call) noexcept { return invoke(call, 2, 2, result_operation); }
FlatwormStatus error_callback(FlatwormCall* call) noexcept { return invoke(call, 3, 4, error_operation); }
FlatwormStatus parse_request_callback(FlatwormCall* call) noexcept { return invoke(call, 1, 1, parse_request_operation); }
FlatwormStatus parse_response_callback(FlatwormCall* call) noexcept { return invoke(call, 1, 2, parse_response_operation); }
FlatwormStatus correlate_callback(FlatwormCall* call) noexcept { return invoke(call, 1, 2, correlate_operation); }

const FlatwormFunction functions[] = {
    {"request", 3, request_callback}, {"notification", 2, notification_callback},
    {"batch", 1, batch_callback}, {"result", 2, result_callback},
    {"error", 4, error_callback}, {"parseRequest", 1, parse_request_callback},
    {"parseResponse", 2, parse_response_callback}, {"correlate", 2, correlate_callback}
};
const FlatwormConstant constants[] = {
    {"JSONRPC_VERSION", {FLATWORM_VALUE_STRING, 0, 0, {"2.0", 3}}},
    {"PARSE_ERROR", {FLATWORM_VALUE_NUMBER, 0, -32700, {nullptr, 0}}},
    {"INVALID_REQUEST", {FLATWORM_VALUE_NUMBER, 0, -32600, {nullptr, 0}}},
    {"METHOD_NOT_FOUND", {FLATWORM_VALUE_NUMBER, 0, -32601, {nullptr, 0}}},
    {"INVALID_PARAMS", {FLATWORM_VALUE_NUMBER, 0, -32602, {nullptr, 0}}},
    {"INTERNAL_ERROR", {FLATWORM_VALUE_NUMBER, 0, -32603, {nullptr, 0}}},
    {"SERVER_ERROR_MIN", {FLATWORM_VALUE_NUMBER, 0, -32099, {nullptr, 0}}},
    {"SERVER_ERROR_MAX", {FLATWORM_VALUE_NUMBER, 0, -32000, {nullptr, 0}}},
    {"MAX_BATCH_SIZE", {FLATWORM_VALUE_NUMBER, 0, static_cast<double>(max_batch_size), {nullptr, 0}}},
    {"MAX_JSON_DEPTH", {FLATWORM_VALUE_NUMBER, 0, static_cast<double>(max_json_depth), {nullptr, 0}}}
};
const FlatwormModuleDefinition definition = {
    FLATWORM_MODULE_ABI_VERSION, sizeof(FlatwormModuleDefinition),
    "rpc", "1.0.0", "Bounded JSON-RPC 2.0 protocol helpers; host-mediated page transport",
    initialize, shutdown, functions, std::size(functions), constants, std::size(constants)
};

}  // namespace

const FlatwormModuleDefinition& module_definition() noexcept { return definition; }

}  // namespace flatworm::rpc
