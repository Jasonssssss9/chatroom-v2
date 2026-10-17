#pragma once

#include "protocol.h"

namespace chatroom {

// Dispatches application requests without accessing sockets or connection buffers.
// Currently handles ping; business operations will be added with their services.
class RequestHandler {
public:
    nlohmann::json handle(std::string_view payload) const;
    nlohmann::json handle(const Request& request) const;
};

} // namespace chatroom
