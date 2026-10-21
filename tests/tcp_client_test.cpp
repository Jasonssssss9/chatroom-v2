#include "net/tcp_client.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
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

// Exercise the real console with piped input, without changing the test runner's stdin.
class ConsoleProcess {
public:
    explicit ConsoleProcess(std::uint16_t port) {
        auto input = pipe();
        auto output = pipe();
        const auto port_text = std::to_string(port);
        pid_ = ::fork();
        check_socket(pid_, "fork");
        if (pid_ == 0) {
            if (::dup2(input[0].get(), STDIN_FILENO) < 0 ||
                ::dup2(output[1].get(), STDOUT_FILENO) < 0) ::_exit(127);
            ::execl(CHATROOM_CLIENT_PATH, CHATROOM_CLIENT_PATH, "127.0.0.1", port_text.c_str(), nullptr);
            ::_exit(127);
        }
        input_ = std::move(input[1]);
        output_ = std::move(output[0]);
    }

    ~ConsoleProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }

    void write(std::string_view bytes) {
        while (!bytes.empty()) {
            const auto count = ::write(input_.get(), bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("Cannot write console input");
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }

    void finish_input() { input_.reset(); }

    const std::string& read_until(std::string_view marker) {
        while (text_.find(marker) == std::string::npos) {
            pollfd event{output_.get(), POLLIN, 0};
            const auto ready = ::poll(&event, 1, 2000);
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) throw std::runtime_error("Timed out waiting for console output");
            char buffer[4096];
            const auto count = ::read(output_.get(), buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("Console exited before the expected output");
            text_.append(buffer, static_cast<std::size_t>(count));
        }
        return text_;
    }

    void check() {
        int status;
        pid_t result;
        do { result = ::waitpid(pid_, &status, 0); } while (result < 0 && errno == EINTR);
        check_socket(result, "waitpid");
        pid_ = -1;
        REQUIRE(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    }

private:
    static std::array<UniqueFd, 2> pipe() {
        int ends[2];
        check_socket(::pipe2(ends, O_CLOEXEC), "pipe2");
        return {UniqueFd(ends[0]), UniqueFd(ends[1])};
    }

    pid_t pid_ = -1;
    UniqueFd input_;
    UniqueFd output_;
    std::string text_;
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

TEST_CASE("Console deduplicates messages but acknowledges every delivery alongside terminal requests",
          "[client][console][ack]") {
    int acknowledgments = 0;
    int requests = 0;
    const auto notice = make_notification("message", {{"message_id", "7"}, {"sender_id", "1"},
        {"client_message_id", "m1"}, {"text", "Display this message once"}}).dump();
    TestPeer peer([&](int fd) {
        for (int i = 0; i < 16; ++i) send_bytes(fd, encode_frame(notice));
        while (acknowledgments < 16 || requests < 8) {
            const auto request = parse_request(receive_payload(fd));
            if (request.action == "ack_message") {
                if (request.data.at("message_id") != "7") throw std::runtime_error("Wrong ACK identity");
                ++acknowledgments;
            } else if (request.action == "ping") {
                if (request.data.at("text") != std::string(16000, 'x')) {
                    throw std::runtime_error("Concurrent sends corrupted a request");
                }
                ++requests;
            } else {
                throw std::runtime_error("Unexpected console request");
            }
        }
        send_bytes(fd, encode_frame(make_response(Request{"done", "ping"}, {{"finished", true}}).dump()));
    });
    ConsoleProcess console(peer.port());
    for (int i = 0; i < 8; ++i) {
        console.write(nlohmann::json{{"type", "request"}, {"request_id", "p" + std::to_string(i)},
            {"action", "ping"}, {"data", {{"text", std::string(16000, 'x')}}}}.dump() + "\n");
    }
    const auto& output = console.read_until("Disconnected.");
    const auto first = output.find("Display this message once");
    REQUIRE(first != std::string::npos);
    CHECK(output.find("Display this message once", first + 1) == std::string::npos);
    console.check();
    peer.check();
    CHECK(acknowledgments == 16);
    CHECK(requests == 8);
}

TEST_CASE("Console displays final messages after stdin EOF without sending on a half-closed socket",
          "[client][console][ack]") {
    TestPeer peer([](int fd) {
        receive_payload(fd);
        char byte;
        if (::recv(fd, &byte, 1, 0) != 0) throw std::runtime_error("Expected stdin EOF half-close");
        send_bytes(fd, encode_frame(make_notification("message", {{"message_id", "8"},
            {"sender_id", "1"}, {"client_message_id", "m1"}, {"text", "Final delivery"}}).dump()));
    });
    ConsoleProcess console(peer.port());
    console.write(R"({"type":"request","request_id":"p","action":"ping"})" "\n");
    console.finish_input();
    const auto& output = console.read_until("Disconnected.");
    CHECK(output.find("Final delivery") != std::string::npos);
    console.check();
    peer.check();
}

TEST_CASE("Console displays malformed notifications without acknowledging them", "[client][console][ack]") {
    TestPeer peer([](int fd) {
        const auto invalid = make_notification("message", {{"message_id", "9"}, {"sender_id", 1},
            {"client_message_id", "m1"}, {"text", "Malformed delivery"}}).dump();
        send_bytes(fd, encode_frame(invalid));
        send_bytes(fd, encode_frame(std::string(65, '[') + "0" + std::string(65, ']')));
        send_bytes(fd, encode_frame(make_response(Request{"marker", "ping"}).dump()));
        if (parse_request(receive_payload(fd)).request_id != "terminal") {
            throw std::runtime_error("Malformed notifications must not be acknowledged");
        }
    });
    ConsoleProcess console(peer.port());
    console.read_until("\"marker\"");
    console.write(R"({"type":"request","request_id":"terminal","action":"ping"})" "\n");
    console.read_until("Disconnected.");
    console.check();
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

TEST_CASE("Console rejects oversized input before newline and preserves the next request", "[client][console]") {
    const std::string payload = R"({"type":"request","request_id":"next","action":"ping"})";
    TestPeer peer([&](int fd) {
        if (receive_payload(fd) != payload) throw std::runtime_error("Console sent discarded input");
        char byte;
        if (::recv(fd, &byte, 1, 0) != 0) throw std::runtime_error("Expected only the valid request");
        send_bytes(fd, encode_frame(payload));
    });
    ConsoleProcess console(peer.port());
    const std::string warning = "Message exceeds the 64 KiB limit.";
    console.write(std::string(kMaxFrameSize + 1, 'x'));
    console.read_until(warning);
    // All of this still belongs to the discarded line, including the apparent quit command.
    console.write(std::string(kMaxFrameSize * 2, 'x') + "/quit\n" + payload + "\n");
    console.finish_input();
    const auto output = console.read_until("Disconnected.");
    CHECK(output.find(warning, output.find(warning) + warning.size()) == std::string::npos);
    CHECK(output.find("< " + payload) != std::string::npos);
    console.check();
    peer.check();
}

TEST_CASE("Console discards an oversized final line at EOF", "[client][console]") {
    TestPeer peer([](int fd) {
        char byte;
        if (::recv(fd, &byte, 1, 0) != 0) throw std::runtime_error("Oversized input reached the peer");
    });
    ConsoleProcess console(peer.port());
    const std::string warning = "Message exceeds the 64 KiB limit.";
    console.write(std::string(kMaxFrameSize * 2, 'x'));
    console.read_until(warning);
    console.finish_input();
    const auto output = console.read_until("Disconnected.");
    CHECK(output.find(warning, output.find(warning) + warning.size()) == std::string::npos);
    console.check();
    peer.check();
}

TEST_CASE("Console accepts exactly 64 KiB with LF, CRLF, or EOF", "[client][console]") {
    std::string ending;
    SECTION("LF") { ending = "\n"; }
    SECTION("CRLF") { ending = "\r\n"; }
    SECTION("EOF") {}
    const std::string payload = "\"" + std::string(kMaxFrameSize - 2, 'x') + "\"";
    TestPeer peer([&](int fd) {
        if (receive_payload(fd) != payload) throw std::runtime_error("Maximum-length payload changed");
        char byte;
        if (::recv(fd, &byte, 1, 0) != 0) throw std::runtime_error("Expected EOF after one frame");
        send_bytes(fd, encode_frame(payload));
    });
    ConsoleProcess console(peer.port());
    console.write(payload + ending);
    console.finish_input();
    const auto output = console.read_until("Disconnected.");
    CHECK(output.find("64 KiB limit") == std::string::npos);
    CHECK(output.find("< " + payload) != std::string::npos);
    console.check();
    peer.check();
}
