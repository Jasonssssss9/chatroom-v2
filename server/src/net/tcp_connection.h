#pragma once

#include "framing.h"
#include "net/socket.h"

#include <cstdint>
#include <string>
#include <vector>

namespace chatroom {

using ConnectionId = std::uint64_t;

// Owns one client's socket and buffers. Access only from the Reactor thread.
class TcpConnection {
public:
    static constexpr std::size_t kMaxPendingBytes = 1024 * 1024;
    static constexpr std::size_t kReceiveBatchSize = 16;

    TcpConnection(Socket socket, std::string peer_address);

    // Returns at most kReceiveBatchSize frames. A full batch requires another call
    // to drain buffered frames and socket input before waiting for an ET event.
    std::vector<std::string> receive();

    // Appends an already encoded frame; returns false if the output limit is exceeded.
    bool enqueue(std::string_view frame);

    // Writes until EAGAIN or completion, retaining the unsent portion.
    void flush();

    int fd() const noexcept { return socket_.fd(); }
    const std::string& peer_address() const noexcept { return peer_address_; }
    bool read_closed() const noexcept { return read_closed_; }
    bool close_deferred() const noexcept { return close_deferred_; }
    void set_close_deferred(bool deferred) noexcept { close_deferred_ = deferred; }
    bool has_pending_output() const noexcept { return sent_ < output_.size(); }
    std::uint32_t events() const noexcept;

private:
    Socket socket_;
    std::string peer_address_;
    FrameDecoder decoder_;
    std::string output_;
    std::size_t sent_ = 0;
    bool read_closed_ = false;
    bool close_deferred_ = false;
};

} // namespace chatroom
