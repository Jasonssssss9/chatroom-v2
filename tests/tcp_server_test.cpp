#include "net/tcp_server.h"

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <future>
#include <sys/socket.h>
#include <thread>

using namespace chatroom;
using namespace std::chrono_literals;

namespace {
// Binds before starting the loop, so clients can connect without timing sleeps.
class RunningServer {
public:
    TcpServer server{0};

    RunningServer() {
        server.set_message_handler([this](ConnectionId id, std::string payload) {
            server.send(id, payload);
        });
    }

    ~RunningServer() { finish(); }

    void start() {
        thread_ = std::thread([this] {
            try {
                server.run();
            } catch (...) {
                failure_ = std::current_exception();
            }
        });
    }

    void finish() {
        if (thread_.joinable()) {
            server.stop();
            thread_.join();
        }
    }

    void check() {
        finish();
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

private:
    std::thread thread_;
    std::exception_ptr failure_;
};

UniqueFd connect_client(std::uint16_t port) {
    UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    REQUIRE(socket.get() >= 0);
    const timeval timeout{2, 0};
    REQUIRE(::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    REQUIRE(::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    REQUIRE(::connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return socket;
}

void send_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        REQUIRE(count > 0);
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
}

std::string receive_exact(int fd, std::size_t size) {
    std::string bytes(size, '\0');
    std::size_t received = 0;
    while (received < size) {
        const auto count = ::recv(fd, bytes.data() + received, size - received, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        REQUIRE(count > 0);
        received += static_cast<std::size_t>(count);
    }
    return bytes;
}

std::string receive_frame(int fd) {
    const auto header = receive_exact(fd, 4);
    std::uint32_t length;
    std::memcpy(&length, header.data(), sizeof(length));
    length = ntohl(length);
    REQUIRE(length <= kMaxFrameSize);
    return receive_exact(fd, length);
}
} // namespace

TEST_CASE("The server echoes framed messages for independent clients", "[server]") {
    RunningServer fixture;
    fixture.start();
    auto first = connect_client(fixture.server.port());
    auto second = connect_client(fixture.server.port());
    send_all(first.get(), encode_frame(R"({"text":"first"})"));
    send_all(second.get(), encode_frame(R"({"text":"second"})"));
    CHECK(receive_frame(first.get()) == R"({"text":"first"})");
    CHECK(receive_frame(second.get()) == R"({"text":"second"})");
    fixture.check();
}

TEST_CASE("The server handles fragmented and consecutive frames", "[server]") {
    RunningServer fixture;
    fixture.start();
    auto client = connect_client(fixture.server.port());
    const auto frame = encode_frame("first");
    send_all(client.get(), frame.substr(0, 2));
    send_all(client.get(), frame.substr(2) + encode_frame("second"));
    CHECK(receive_frame(client.get()) == "first");
    CHECK(receive_frame(client.get()) == "second");
    fixture.check();
}

TEST_CASE("Final messages are echoed before a half-closed connection closes", "[server]") {
    RunningServer fixture;
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(), encode_frame("first") + encode_frame("second"));
    REQUIRE(::shutdown(client.get(), SHUT_WR) == 0);
    CHECK(receive_frame(client.get()) == "first");
    CHECK(receive_frame(client.get()) == "second");
    char byte;
    CHECK(::recv(client.get(), &byte, 1, 0) == 0);
    fixture.check();
}

TEST_CASE("An oversized frame closes only the offending connection", "[server]") {
    RunningServer fixture;
    fixture.start();
    auto invalid = connect_client(fixture.server.port());
    const auto length = htonl(static_cast<std::uint32_t>(kMaxFrameSize + 1));
    send_all(invalid.get(),
             std::string_view(reinterpret_cast<const char*>(&length), sizeof(length)));
    char byte;
    CHECK(::recv(invalid.get(), &byte, 1, 0) == 0);

    auto valid = connect_client(fixture.server.port());
    send_all(valid.get(), encode_frame("still running"));
    CHECK(receive_frame(valid.get()) == "still running");
    fixture.check();
}

TEST_CASE("Late replies for a disconnected client do not reach a new client", "[server]") {
    std::promise<ConnectionId> captured;
    auto captured_id = captured.get_future();
    std::promise<void> disconnected;
    auto closed = disconnected.get_future();
    ConnectionId old_id = 0;
    RunningServer fixture;
    fixture.server.set_message_handler([&](ConnectionId id, std::string payload) {
        if (payload == "capture") {
            old_id = id;
            captured.set_value(id);
        } else {
            fixture.server.send(id, payload);
        }
    });
    fixture.server.set_disconnect_handler([&](ConnectionId id) {
        if (id == old_id) {
            disconnected.set_value();
        }
    });
    fixture.start();
    auto first = connect_client(fixture.server.port());
    send_all(first.get(), encode_frame("capture"));
    REQUIRE(captured_id.wait_for(2s) == std::future_status::ready);
    const auto id = captured_id.get();
    first.reset();
    REQUIRE(closed.wait_for(2s) == std::future_status::ready);

    auto second = connect_client(fixture.server.port());
    fixture.server.send(id, "stale reply");
    send_all(second.get(), encode_frame("fresh reply"));
    CHECK(receive_frame(second.get()) == "fresh reply");
    fixture.check();
}

TEST_CASE("Stop wakes an idle event loop", "[server]") {
    RunningServer fixture;
    fixture.start();
    fixture.check();
}

TEST_CASE("A reply submitted from another thread wakes the event loop", "[server]") {
    std::promise<ConnectionId> captured;
    auto id = captured.get_future();
    RunningServer fixture;
    fixture.server.set_message_handler([&](ConnectionId connection, std::string) {
        captured.set_value(connection);
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(), encode_frame("request"));
    REQUIRE(id.wait_for(2s) == std::future_status::ready);
    fixture.server.send(id.get(), "worker reply");
    CHECK(receive_frame(client.get()) == "worker reply");
    fixture.check();
}

TEST_CASE("Listener and accepted sockets are non-blocking and close on exec", "[server]") {
    auto listener = Socket::listen_on("127.0.0.1", 0);
    CHECK((::fcntl(listener.fd(), F_GETFL) & O_NONBLOCK) != 0);
    CHECK((::fcntl(listener.fd(), F_GETFD) & FD_CLOEXEC) != 0);
    auto client = connect_client(listener.local_port());
    std::string peer;
    auto accepted = listener.accept(peer);
    REQUIRE(accepted.fd() >= 0);
    CHECK((::fcntl(accepted.fd(), F_GETFL) & O_NONBLOCK) != 0);
    CHECK((::fcntl(accepted.fd(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(peer.find("127.0.0.1:") == 0);
}
