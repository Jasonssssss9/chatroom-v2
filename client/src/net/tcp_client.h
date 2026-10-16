#pragma once

#include "framing.h"
#include "unique_fd.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace chatroom {

// Owns one blocking IPv4 connection. One sender and one receiver may run concurrently.
// Join both before destruction. Reconnecting requires a new instance.
class TcpClient {
public:
    TcpClient(const std::string& address, std::uint16_t port);

    // Sends a complete frame, retrying interrupted and partial writes.
    void send(std::string_view payload);

    // Blocks for one frame. Returns no value on EOF; truncated frames are errors.
    std::optional<std::string> receive();

    // Sends FIN while keeping the receive side open for final replies.
    void finish_sending();

    // Cancels blocked I/O. The descriptor stays owned until destruction.
    void disconnect() noexcept;

private:
    UniqueFd socket_;
    FrameDecoder decoder_;
};

} // namespace chatroom
