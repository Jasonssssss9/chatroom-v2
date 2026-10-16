#include "net/tcp_client.h"

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <utility>

using namespace chatroom;
using namespace std::chrono_literals;

namespace {
void check_socket(int result, const char* operation) {
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), operation);
    }
}

void send_bytes(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Test peer could not send data");
        }
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
}

std::string receive_bytes(int fd, std::size_t size) {
    std::string bytes(size, '\0');
    std::size_t received = 0;
    while (received < size) {
        const auto count = ::recv(fd, bytes.data() + received, size - received, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Test peer received an incomplete frame");
        }
        received += static_cast<std::size_t>(count);
    }
    return bytes;
}

std::string receive_payload(int fd) {
    const auto header = receive_bytes(fd, 4);
    std::uint32_t length;
    std::memcpy(&length, header.data(), sizeof(length));
    length = ntohl(length);
    if (length > kMaxFrameSize) {
        throw std::runtime_error("Test peer received an oversized frame");
    }
    return receive_bytes(fd, length);
}

// A raw TCP peer keeps client tests independent of the server build.
class TestPeer {
public:
    explicit TestPeer(std::function<void(int)> handler)
        : listener_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
        check_socket(listener_.get(), "socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check_socket(::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
                            sizeof(address)),
                     "bind");
        check_socket(::listen(listener_.get(), 1), "listen");
        socklen_t size = sizeof(address);
        check_socket(::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &size),
                     "getsockname");
        port_ = ntohs(address.sin_port);

        thread_ = std::thread([this, handler = std::move(handler)] {
            try {
                UniqueFd socket(::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC));
                check_socket(socket.get(), "accept4");
                const timeval timeout{2, 0};
                check_socket(::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                                          sizeof(timeout)), "setsockopt");
                check_socket(::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                                          sizeof(timeout)), "setsockopt");
                handler(socket.get());
            } catch (...) {
                error_ = std::current_exception();
            }
        });
    }

    ~TestPeer() {
        if (thread_.joinable()) {
            ::shutdown(listener_.get(), SHUT_RDWR);
            thread_.join();
        }
    }

    std::uint16_t port() const { return port_; }

    void check() {
        thread_.join();
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

private:
    UniqueFd listener_;
    std::uint16_t port_;
    std::thread thread_;
    std::exception_ptr error_;
};
} // namespace

TEST_CASE("The client sends frames and retains consecutive replies", "[client]") {
    const std::string payload = std::string(u8"\u4f60\u597d") + std::string("\0end", 4);
    TestPeer peer([&](int fd) {
        const auto received = receive_payload(fd);
        send_bytes(fd, encode_frame(received) + encode_frame("next"));
    });
    TcpClient client("127.0.0.1", peer.port());
    client.send(payload);
    CHECK(client.receive() == payload);
    CHECK(client.receive() == "next");
    CHECK_FALSE(client.receive().has_value());
    peer.check();
}

TEST_CASE("The client receives fragmented replies and empty frames", "[client]") {
    TestPeer peer([](int fd) {
        const auto frame = encode_frame("fragmented");
        send_bytes(fd, frame.substr(0, 2));
        send_bytes(fd, frame.substr(2, 3));
        send_bytes(fd, frame.substr(5) + encode_frame(""));
    });
    TcpClient client("127.0.0.1", peer.port());
    CHECK(client.receive() == "fragmented");
    CHECK(client.receive() == "");
    CHECK_FALSE(client.receive().has_value());
    peer.check();
}

TEST_CASE("The client reports EOF in an incomplete frame", "[client]") {
    const auto frame = encode_frame("message");
    std::string incomplete;
    SECTION("Partial header") { incomplete = frame.substr(0, 2); }
    SECTION("Partial payload") { incomplete = frame.substr(0, 6); }
    TestPeer peer([&](int fd) { send_bytes(fd, incomplete); });
    TcpClient client("127.0.0.1", peer.port());
    CHECK_THROWS_WITH(client.receive(), "Connection closed in the middle of a frame");
    peer.check();
}

TEST_CASE("The client rejects oversized reply headers", "[client]") {
    TestPeer peer([](int fd) {
        const auto length = htonl(static_cast<std::uint32_t>(kMaxFrameSize + 1));
        send_bytes(fd, std::string_view(reinterpret_cast<const char*>(&length), sizeof(length)));
    });
    TcpClient client("127.0.0.1", peer.port());
    CHECK_THROWS_AS(client.receive(), std::length_error);
    peer.check();
}

TEST_CASE("An oversized send does not write a partial frame", "[client]") {
    TestPeer peer([](int fd) { send_bytes(fd, encode_frame(receive_payload(fd))); });
    TcpClient client("127.0.0.1", peer.port());
    CHECK_THROWS_AS(client.send(std::string(kMaxFrameSize + 1, 'x')), std::length_error);
    const std::string payload(kMaxFrameSize, 'x');
    client.send(payload);
    CHECK(client.receive() == payload);
    peer.check();
}

TEST_CASE("Finishing sends still allows the client to receive final replies", "[client]") {
    TestPeer peer([](int fd) {
        const auto payload = receive_payload(fd);
        char byte;
        if (::recv(fd, &byte, 1, 0) != 0) {
            throw std::runtime_error("Test peer expected EOF");
        }
        send_bytes(fd, encode_frame(payload));
    });
    TcpClient client("127.0.0.1", peer.port());
    client.send("final message");
    client.finish_sending();
    CHECK(client.receive() == "final message");
    CHECK_FALSE(client.receive().has_value());
    peer.check();
}

TEST_CASE("Disconnect cancels a blocking receive from another thread", "[client]") {
    TestPeer peer([](int fd) {
        char byte;
        if (::recv(fd, &byte, 1, 0) != 0) {
            throw std::runtime_error("Test peer expected EOF");
        }
    });
    TcpClient client("127.0.0.1", peer.port());
    auto reply = std::async(std::launch::async, [&] { return client.receive(); });
    client.disconnect();
    REQUIRE(reply.wait_for(2s) == std::future_status::ready);
    CHECK_FALSE(reply.get().has_value());
    peer.check();
}

TEST_CASE("The client reports invalid addresses and refused connections", "[client]") {
    CHECK_THROWS_AS(TcpClient("invalid", 8081), std::invalid_argument);

    UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    REQUIRE(socket.get() >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    REQUIRE(::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &size) == 0);
    // The port stays reserved, but no listener is started.
    CHECK_THROWS_AS(TcpClient("127.0.0.1", ntohs(address.sin_port)), std::system_error);
}
