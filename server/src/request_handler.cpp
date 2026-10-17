#include "request_handler.h"

namespace chatroom {

nlohmann::json RequestHandler::handle(std::string_view payload) const {
    try {
        return handle(parse_request(payload));
    } catch (const ProtocolError& error) {
        return make_error_response(error.request, error.code, error.what());
    }
}

nlohmann::json RequestHandler::handle(const Request& request) const {
    if (request.action == "ping") {
        return make_response(request, {{"pong", true}});
    }
    return make_error_response(request, "unknown_action", "Action is not supported");
}

} // namespace chatroom
