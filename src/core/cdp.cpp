#include "prowsetk/cdp.hpp"
#include "protocol_json.hpp"
#include <algorithm>
#include <cctype>

namespace prowsetk {
using namespace protocol;
CdpClient::CdpClient(std::unique_ptr<WebSocket> transport) : transport_(std::move(transport)) {
    if (!transport_) throw Error(ErrorCode::InvalidArgument, "CDP requires transport");
}
CdpClient::~CdpClient() = default;
std::string CdpClient::call(std::string_view method, std::string_view params, std::string_view session_id) {
    if (active_) throw Error(ErrorCode::InvalidArgument, "CDP command reentry rejected");
    struct Guard { bool& active; explicit Guard(bool& value) : active(value) { active = true; } ~Guard() { active = false; } } guard(active_);
    if (!transport_) throw Error(ErrorCode::InvalidArgument, "CDP connection closed");
    if (method.empty() || method.size() > 256 || method.find('.') == std::string_view::npos ||
        !std::all_of(method.begin(), method.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_'; }) ||
        params.size() > 1024 * 1024 || session_id.size() > 256)
        throw Error(ErrorCode::InvalidArgument, "invalid CDP command");
    auto arguments = parse(params);
    if (arguments.kind != Json::Kind::Object) throw Error(ErrorCode::InvalidArgument, "CDP params must be an object");
    if (++next_id_ > flatworm::rpc::max_safe_integer) throw Error(ErrorCode::ResourceLimit, "CDP identifier limit");
    auto request = Json::object_value();
    request.put("id", Json::number_value(static_cast<double>(next_id_)));
    put(request, "method", method);
    request.put("params", std::move(arguments));
    if (!session_id.empty()) put(request, "sessionId", session_id);
    try {
        transport_->send(encode(request));
        for (unsigned i = 0; i < 1024; ++i) {
            auto wire = transport_->receive();
            if (wire.size() > 1024 * 1024) throw Error(ErrorCode::ResourceLimit, "CDP message limit");
            const auto reply = parse(wire);
            if (reply.kind != Json::Kind::Object) throw Error(ErrorCode::ParseError, "invalid CDP envelope");
            if (const auto* id = reply.find("id")) {
                const auto* sid = reply.find("sessionId");
                if (id->kind != Json::Kind::Number || id->text != std::to_string(next_id_) ||
                    (!session_id.empty() && (!sid || sid->kind != Json::Kind::String || sid->text != session_id)) ||
                    (session_id.empty() && sid))
                    throw Error(ErrorCode::ParseError, "CDP response correlation failed");
                if (reply.find("error")) throw Error(ErrorCode::Unsupported, "CDP command rejected by peer");
                const auto* result = reply.find("result");
                if (!result || result->kind != Json::Kind::Object) throw Error(ErrorCode::ParseError, "invalid CDP result");
                return encode(*result);
            }
            (void)string(reply, "method");
            if (events_.size() >= 1024 || wire.size() > 4 * 1024 * 1024 - event_bytes_)
                throw Error(ErrorCode::ResourceLimit, "CDP event queue limit");
            event_bytes_ += wire.size();
            events_.push_back(std::move(wire));
        }
        throw Error(ErrorCode::ResourceLimit, "CDP response budget exceeded");
    } catch (const Error& error) {
        transport_.reset();
        throw Error(error.code(), "CDP operation failed");
    } catch (...) {
        transport_.reset();
        throw Error(ErrorCode::NetworkError, "CDP operation failed");
    }
}
std::vector<std::string> CdpClient::take_events() {
    event_bytes_ = 0;
    auto result = std::move(events_);
    events_.clear();
    return result;
}
std::unique_ptr<WebSocket> NetworkClient::open_websocket(const HttpRequest&) {
    throw Error(ErrorCode::Unsupported, "WebSocket transport unavailable");
}
}
