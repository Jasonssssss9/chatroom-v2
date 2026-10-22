#pragma once

#include "db/chat_store.h"
#include "protocol.h"

namespace chatroom {

// A worker result; online routing belongs to the Reactor.
struct UserNotification {
    std::vector<std::uint64_t> recipients;
    std::string payload;
};

inline nlohmann::json make_file_notification(std::uint64_t message, const FileInfo& file) {
    return make_notification("file_available", {{"message_id", std::to_string(message)},
        {"file_id", std::to_string(file.id)}, {"sender_id", std::to_string(file.owner_id)},
        {"name", file.original_name}, {"content_type", file.content_type}, {"size_bytes", file.size_bytes}});
}

} // namespace chatroom
