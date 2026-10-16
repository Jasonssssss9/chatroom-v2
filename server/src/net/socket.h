#pragma once

#include "unique_fd.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>

namespace chatroom {

// Owns a socket and provides thin wrappers around the Linux socket API.
class Socket {
public:
    explicit Socket(UniqueFd fd) noexcept : fd_(std::move(fd)) {}

    static Socket listen_on(const std::string& address, std::uint16_t port);

    // Returns a socket with fd() == -1 on failure, preserving accept4's errno.
    Socket accept(std::string& peer_address) const;

    // Preserve recv/send return values and errno, including EAGAIN and EINTR.
    ssize_t receive(void* buffer, std::size_t size) const;
    ssize_t send(std::string_view bytes) const;

    int fd() const noexcept { return fd_.get(); }
    std::uint16_t local_port() const;

private:
    UniqueFd fd_;
};

} // namespace chatroom
