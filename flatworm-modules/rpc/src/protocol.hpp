#ifndef FLATWORM_RPC_PROTOCOL_HPP
#define FLATWORM_RPC_PROTOCOL_HPP

#include "json.hpp"

namespace flatworm::rpc {

void validate_id(const Json& id);
void validate_request(const Json& value);
void validate_requests(const Json& value);
void validate_response(const Json& value);
void validate_responses(const Json& value);
bool same_id(const Json& left, const Json& right);
Json request(std::string_view method, const Json* params, const Json* id);
Json result(const Json& id, Json value);
Json error(const Json& id, const Json& code, std::string_view message, const Json* data);
Json batch(Json requests);
Json correlate(const Json& requests, const Json* responses);

}  // namespace flatworm::rpc

#endif
