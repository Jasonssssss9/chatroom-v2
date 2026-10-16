#include "net/tcp_client.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>

namespace chatroom {

TcpClient::TcpClient(const std::string& address, std::uint16_t port)
    : socket_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    if (socket_.get() < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &peer.sin_addr) != 1) {
        throw std::invalid_argument("Invalid IPv4 server address: " + address);
    }
    if (::connect(socket_.get(), reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) < 0) {
        throw std::system_error(errno, std::generic_category(), "connect");
    }
}

void TcpClient::send(std::string_view payload) {
    const auto frame = encode_frame(payload);
    std::string_view remaining(frame);
    while (!remaining.empty()) {
        const auto count = ::send(socket_.get(), remaining.data(), remaining.size(), MSG_NOSIGNAL);
        if (count > 0) {
            remaining.remove_prefix(static_cast<std::size_t>(count));
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::system_error(count == 0 ? EPIPE : errno, std::generic_category(), "send");
        }
    }
}

std::optional<std::string> TcpClient::receive() {
    char buffer[8192];
    while (true) {
        if (auto frame = decoder_.next()) {
            return frame;
        }
        const auto count = ::recv(socket_.get(), buffer, sizeof(buffer), 0);
        if (count > 0) {
            decoder_.append(std::string_view(buffer, static_cast<std::size_t>(count)));
        } else if (count == 0) {
            if (!decoder_.empty()) {
                throw std::runtime_error("Connection closed in the middle of a frame");
            }
            return std::nullopt;
        } else if (errno == EINTR) {
            continue;
        } else {
            throw std::system_error(errno, std::generic_category(), "recv");
        }
    }
}

void TcpClient::finish_sending() {
    if (::shutdown(socket_.get(), SHUT_WR) < 0) {
        throw std::system_error(errno, std::generic_category(), "shutdown(SHUT_WR)");
    }
}

void TcpClient::disconnect() noexcept {
    // Cancellation is also used during cleanup after the peer has already closed.
    ::shutdown(socket_.get(), SHUT_RDWR);
}

} // namespace chatroom
