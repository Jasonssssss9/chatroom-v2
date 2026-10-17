#include "net/tcp_connection.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <system_error>
#include <utility>

using namespace chatroom;

namespace {
std::array<UniqueFd, 2> socket_pair() {
    int pair[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    return {UniqueFd(pair[0]), UniqueFd(pair[1])};
}
} // namespace

TEST_CASE("File descriptor ownership moves and closes exactly once", "[net]") {
    auto pair = socket_pair();
    const int fd = pair[0].get();
    {
        UniqueFd owner(std::move(pair[0]));
        CHECK(pair[0].get() == -1);
        CHECK(owner.get() == fd);
        UniqueFd other;
        other = std::move(owner);
        CHECK(owner.get() == -1);
        CHECK(other.get() == fd);
    }
    CHECK(::fcntl(fd, F_GETFD) == -1);
    CHECK(errno == EBADF);
}

TEST_CASE("A connection returns complete messages received before EOF", "[net]") {
    auto pair = socket_pair();
    TcpConnection connection(Socket(std::move(pair[0])), "local");
    const auto bytes = encode_frame("first") + encode_frame(std::string("a\0b", 3));
    REQUIRE(::send(pair[1].get(), bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(bytes.size()));
    REQUIRE(::shutdown(pair[1].get(), SHUT_WR) == 0);
    const auto frames = connection.receive();
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == "first");
    CHECK(frames[1] == std::string("a\0b", 3));
    CHECK(connection.read_closed());
    CHECK_FALSE(connection.truncated_frame());
}

TEST_CASE("Truncated EOF preserves complete frames and records incomplete input", "[net][eof]") {
    auto tail = encode_frame("unfinished");
    SECTION("Incomplete frame header") { tail.resize(2); }
    SECTION("Incomplete frame payload") { tail.pop_back(); }

    for (const int complete_count : {0, 1, 16, 20}) {
        INFO("Complete frames before truncation: " << complete_count);
        auto pair = socket_pair();
        TcpConnection connection(Socket(std::move(pair[0])), "local");
        std::string bytes;
        for (int i = 0; i < complete_count; ++i) bytes += encode_frame(std::to_string(i));
        bytes += tail;
        REQUIRE(::send(pair[1].get(), bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(bytes.size()));
        REQUIRE(::shutdown(pair[1].get(), SHUT_WR) == 0);
        int received = 0;
        for (int pass = 0; pass < 4 && !connection.read_closed(); ++pass) {
            for (const auto& frame : connection.receive()) {
                CHECK(frame == std::to_string(received));
                ++received;
            }
        }
        CHECK(received == complete_count);
        CHECK(connection.read_closed());
        CHECK(connection.truncated_frame());
    }
}

TEST_CASE("Receive batches bound temporary storage and preserve buffered frames", "[net]") {
    auto pair = socket_pair();
    TcpConnection connection(Socket(std::move(pair[0])), "local");
    std::string bytes;
    constexpr int message_count = 50;
    for (int i = 0; i < message_count; ++i) bytes += encode_frame(std::to_string(i));
    REQUIRE(::send(pair[1].get(), bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(bytes.size()));
    REQUIRE(::shutdown(pair[1].get(), SHUT_WR) == 0);
    int received = 0;
    for (int pass = 0; pass < message_count && !connection.read_closed(); ++pass) {
        const auto frames = connection.receive();
        REQUIRE(frames.size() <= TcpConnection::kReceiveBatchSize);
        for (const auto& frame : frames) {
            CHECK(frame == std::to_string(received));
            ++received;
        }
    }
    CHECK(received == message_count);
    CHECK(connection.read_closed());
    CHECK_FALSE(connection.truncated_frame());
}

TEST_CASE("A connection retains partial output until its peer reads", "[net]") {
    auto pair = socket_pair();
    const int capacity = 4096;
    REQUIRE(::setsockopt(pair[0].get(), SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
    TcpConnection connection(Socket(std::move(pair[0])), "local");
    const std::string frame = encode_frame(std::string(kMaxFrameSize, 'x'));
    const std::string expected = frame + frame + frame;
    REQUIRE(connection.enqueue(expected));
    connection.flush();
    REQUIRE(connection.has_pending_output());

    std::string received;
    char buffer[8192];
    // Each pass drains the peer before retrying the blocked writer.
    for (int pass = 0; pass < 1000 && received.size() < expected.size(); ++pass) {
        while (true) {
            const auto count = ::recv(pair[1].get(), buffer, sizeof(buffer), 0);
            if (count > 0) {
                received.append(buffer, static_cast<std::size_t>(count));
            } else {
                REQUIRE(count < 0);
                REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
                break;
            }
        }
        connection.flush();
    }
    CHECK(received == expected);
    CHECK_FALSE(connection.has_pending_output());
    CHECK((connection.events() & EPOLLOUT) == 0);
}

TEST_CASE("A connection limits queued output", "[net]") {
    auto pair = socket_pair();
    TcpConnection connection(Socket(std::move(pair[0])), "local");
    REQUIRE(connection.enqueue(std::string(TcpConnection::kMaxPendingBytes, 'x')));
    CHECK_FALSE(connection.enqueue("x"));
}

TEST_CASE("Sending to a closed peer reports an error without SIGPIPE", "[net]") {
    auto pair = socket_pair();
    TcpConnection connection(Socket(std::move(pair[0])), "local");
    pair[1].reset();
    REQUIRE(connection.enqueue(encode_frame("message")));
    CHECK_THROWS_AS(connection.flush(), std::system_error);
}
