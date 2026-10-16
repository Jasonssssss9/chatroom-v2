#include "net/socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>

namespace chatroom {

Socket Socket::listen_on(const std::string& address, std::uint16_t port) {
    Socket socket(UniqueFd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)));
    if (socket.fd() < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    const int reuse = 1;
    if (::setsockopt(socket.fd(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        throw std::system_error(errno, std::generic_category(), "setsockopt(SO_REUSEADDR)");
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &local.sin_addr) != 1) {
        throw std::invalid_argument("Invalid IPv4 bind address: " + address);
    }
    if (::bind(socket.fd(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0) {
        throw std::system_error(errno, std::generic_category(), "bind");
    }
    if (::listen(socket.fd(), SOMAXCONN) < 0) {
        throw std::system_error(errno, std::generic_category(), "listen");
    }
    return socket;
}

Socket Socket::accept(std::string& peer_address) const {
    sockaddr_in peer{};
    socklen_t size = sizeof(peer);
    Socket socket(UniqueFd(::accept4(fd(), reinterpret_cast<sockaddr*>(&peer), &size,
                                     SOCK_NONBLOCK | SOCK_CLOEXEC)));
    if (socket.fd() >= 0) {
        char address[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
        peer_address = std::string(address) + ":" + std::to_string(ntohs(peer.sin_port));
    }
    return socket;
}

ssize_t Socket::receive(void* buffer, std::size_t size) const {
    return ::recv(fd(), buffer, size, 0);
}

ssize_t Socket::send(std::string_view bytes) const {
    return ::send(fd(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
}

std::uint16_t Socket::local_port() const {
    sockaddr_in local{};
    socklen_t size = sizeof(local);
    if (::getsockname(fd(), reinterpret_cast<sockaddr*>(&local), &size) < 0) {
        throw std::system_error(errno, std::generic_category(), "getsockname");
    }
    return ntohs(local.sin_port);
}

} // namespace chatroom
