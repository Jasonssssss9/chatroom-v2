#include "net/tcp_server.h"
#include "logger.h"
#include "request_handler.h"
#include "request_dispatcher.h"
#include "task_gate.h"

#include <arpa/inet.h>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>

using namespace chatroom;
using namespace std::chrono_literals;

namespace {
const TokenService kTestTokens(std::string(32, 'k'));

// Binds before starting the loop, so clients can connect without timing sleeps.
class RunningServer {
public:
    TcpServer server;
    std::unique_ptr<RequestDispatcher> dispatcher;

    explicit RunningServer(std::size_t max_connections = 1024)
        : server(0, "127.0.0.1", max_connections) {
        server.set_message_handler([this](ConnectionId id, std::string payload) {
            server.send(id, payload);
        });
    }

    ~RunningServer() { finish(); }

    void use_workers(std::function<nlohmann::json(ConnectionId, const Request&)> handler, std::size_t worker_count = 2,
                     std::size_t per_connection = 32, std::size_t total = 1024) {
        use_session_workers(
            [handler = std::move(handler)](ConnectionId id, const Request& request, std::optional<Session>) {
                return RequestResult{handler(id, request)};
            }, worker_count, per_connection, total);
    }

    void use_session_workers(RequestDispatcher::Handler handler, std::size_t worker_count = 2,
                             std::size_t per_connection = 32, std::size_t total = 1024) {
        dispatcher = std::make_unique<RequestDispatcher>(server, std::move(handler), worker_count,
                                                         per_connection, total);
        server.set_message_handler([this](ConnectionId id, std::string payload) {
            dispatcher->enqueue(id, std::move(payload));
        });
        server.set_disconnect_handler([this](ConnectionId id) { dispatcher->disconnect(id); });
    }

    void start() {
        thread_ = std::thread([this] {
            try {
                server.run();
            } catch (...) {
                failure_ = std::current_exception();
            }
        });
    }

    void stop_loop() {
        if (thread_.joinable()) {
            server.stop();
            thread_.join();
        }
    }

    void finish() {
        stop_loop();
        dispatcher.reset();
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

// Construct before starting the Reactor and restore the stream after all threads join.
class LogCapture {
public:
    LogCapture() : previous_(std::clog.rdbuf(output.rdbuf())) {}
    ~LogCapture() { std::clog.rdbuf(previous_); }

    std::ostringstream output;

private:
    std::streambuf* previous_;
};

void check_eof_logs(const std::string& text, bool truncated) {
    std::istringstream lines(text);
    std::string line;
    ConnectionId first_id = 0;
    std::string first_peer;
    int errors = 0;
    int closes = 0;
    while (std::getline(lines, line)) {
        const auto entry = nlohmann::json::parse(line);
        const auto& context = entry.at("context");
        if (entry.at("event") == "connection_opened" && first_id == 0) {
            first_id = context.at("connection_id").get<ConnectionId>();
            first_peer = context.at("peer").get<std::string>();
        }
        if (first_id == 0 || context.value("connection_id", ConnectionId{0}) != first_id) continue;
        if (entry.at("event") == "connection_error") {
            ++errors;
            CHECK(entry.at("level") == "warn");
            CHECK(context.at("code") == "truncated_frame");
            CHECK(context.at("peer") == first_peer);
            CHECK(context.at("error") == "Connection closed in the middle of a frame");
        }
        if (entry.at("event") == "connection_closed") {
            ++closes;
            CHECK(context.at("reason") == (truncated ? "truncated_frame" : "peer_closed"));
        }
    }
    REQUIRE(first_id != 0);
    CHECK(errors == (truncated ? 1 : 0));
    CHECK(closes == 1);
}

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

void expect_closed(int fd) {
    char byte;
    const auto count = ::recv(fd, &byte, 1, 0);
    CHECK((count == 0 || (count < 0 && errno == ECONNRESET)));
}

// Keep FD-limit changes in a child, and reap it even if a test assertion fails.
class ChildProcess {
public:
    explicit ChildProcess(pid_t pid) : pid_(pid) {}
    ~ChildProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }

    int wait() {
        int status;
        pid_t result;
        do { result = ::waitpid(pid_, &status, 0); } while (result < 0 && errno == EINTR);
        REQUIRE(result == pid_);
        pid_ = -1;
        return status;
    }

private:
    pid_t pid_;
};

nlohmann::json await_log(int fd, std::string_view event) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string line;
        char byte;
        do {
            byte = receive_exact(fd, 1).front();
            line += byte;
        } while (byte != '\n');
        const auto entry = nlohmann::json::parse(line);
        if (entry.at("event") == event) return entry;
    }
    FAIL("Timed out waiting for log event: " << event);
    return {};
}

void run_resource_limited_server() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) < 0) {
        throw std::system_error(errno, std::generic_category(), "getrlimit");
    }
    if (limit.rlim_cur > 64) limit.rlim_cur = 64;
    if (::setrlimit(RLIMIT_NOFILE, &limit) < 0) {
        throw std::system_error(errno, std::generic_category(), "setrlimit");
    }
    TcpServer server(0);
    std::vector<UniqueFd> occupied;
    server.set_message_handler([&](ConnectionId id, std::string payload) {
        if (payload == "exhaust") {
            int fd;
            while ((fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC)) >= 0) {
                occupied.emplace_back(fd);
            }
            if (errno != EMFILE) throw std::runtime_error("Expected EMFILE in test child");
            server.send(id, "exhausted");
        } else if (payload == "release") {
            occupied.clear();
            server.send(id, "released");
        } else if (payload == "stop") {
            server.stop();
        } else {
            server.send(id, payload);
        }
    });
    log(LogLevel::info, "test_server_ready", {{"port", server.port()}});
    server.run();
    log(LogLevel::info, "test_server_stopped");
}
} // namespace

TEST_CASE("Connection limits preserve active clients and allow later connections", "[server]") {
    std::promise<void> disconnected;
    auto closed = disconnected.get_future();
    RunningServer fixture(1);
    fixture.server.set_disconnect_handler([&, notified = false](ConnectionId) mutable {
        if (!notified) {
            notified = true;
            disconnected.set_value();
        }
    });
    fixture.start();
    auto first = connect_client(fixture.server.port());
    send_all(first.get(), encode_frame("accepted"));
    CHECK(receive_frame(first.get()) == "accepted");

    auto rejected = connect_client(fixture.server.port());
    char byte;
    CHECK(::recv(rejected.get(), &byte, 1, 0) == 0);
    send_all(first.get(), encode_frame("still running"));
    CHECK(receive_frame(first.get()) == "still running");
    first.reset();
    REQUIRE(closed.wait_for(2s) == std::future_status::ready);

    auto replacement = connect_client(fixture.server.port());
    send_all(replacement.get(), encode_frame("replacement"));
    CHECK(receive_frame(replacement.get()) == "replacement");
    fixture.check();
}

TEST_CASE("FD exhaustion pauses accepting while established connections keep working",
          "[server][accept]") {
    bool release_resources = true;
    SECTION("Accepting recovers for a connection already waiting in the backlog") {}
    SECTION("Stop wakes the Reactor while accepting is paused") { release_resources = false; }

    int logs[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, logs) == 0);
    UniqueFd reader(logs[0]);
    UniqueFd writer(logs[1]);
    const timeval timeout{2, 0};
    REQUIRE(::setsockopt(reader.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    const auto pid = ::fork();
    if (pid == 0) {
        reader.reset();
        if (::dup2(writer.get(), STDERR_FILENO) < 0) std::_Exit(2);
        writer.reset();
        try {
            run_resource_limited_server();
            std::_Exit(0);
        } catch (const std::exception& error) {
            log(LogLevel::error, "test_server_failed", {{"error", error.what()}});
            std::_Exit(1);
        }
    }
    REQUIRE(pid > 0);
    ChildProcess child(pid);
    writer.reset();
    const auto ready = await_log(reader.get(), "test_server_ready");
    const auto port = ready.at("context").at("port").get<std::uint16_t>();
    auto existing = connect_client(port);
    send_all(existing.get(), encode_frame("exhaust"));
    CHECK(receive_frame(existing.get()) == "exhausted");

    auto waiting = connect_client(port);
    send_all(waiting.get(), encode_frame("waiting"));
    const auto paused = await_log(reader.get(), "accept_paused");
    CHECK(paused.at("context").at("errno") == EMFILE);
    CHECK(paused.at("context").at("retry_ms") == 200);
    send_all(existing.get(), encode_frame("still running"));
    CHECK(receive_frame(existing.get()) == "still running");
    // A second pause proves that an unsuccessful retry also schedules another attempt.
    await_log(reader.get(), "accept_paused");

    if (release_resources) {
        send_all(existing.get(), encode_frame("release"));
        CHECK(receive_frame(existing.get()) == "released");
        CHECK(receive_frame(waiting.get()) == "waiting");
        auto fresh = connect_client(port);
        send_all(fresh.get(), encode_frame("fresh"));
        CHECK(receive_frame(fresh.get()) == "fresh");
    }
    send_all(existing.get(), encode_frame("stop"));
    await_log(reader.get(), "test_server_stopped");
    const int status = child.wait();
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

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

TEST_CASE("EOF distinguishes truncated input from an orderly half-close", "[server][eof]") {
    auto tail = encode_frame("unfinished");
    bool truncated = true;
    SECTION("Incomplete frame header") { tail.resize(2); }
    SECTION("Incomplete frame payload") { tail.pop_back(); }
    SECTION("No incomplete frame") {
        tail.clear();
        truncated = false;
    }
    LogCapture logs;
    RunningServer fixture;
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(), tail);
    REQUIRE(::shutdown(client.get(), SHUT_WR) == 0);
    char byte;
    CHECK(::recv(client.get(), &byte, 1, 0) == 0);

    auto other = connect_client(fixture.server.port());
    send_all(other.get(), encode_frame("still running"));
    CHECK(receive_frame(other.get()) == "still running");
    fixture.check();
    check_eof_logs(logs.output.str(), truncated);
}

TEST_CASE("Complete worker requests finish before closing a truncated stream",
          "[server][dispatcher][eof]") {
    auto tail = encode_frame("unfinished");
    int request_count = 20;
    SECTION("Incomplete header after a full receive batch") {
        request_count = static_cast<int>(TcpConnection::kReceiveBatchSize);
        tail.resize(2);
    }
    SECTION("Incomplete payload after several receive batches") { tail.pop_back(); }
    std::promise<void> started;
    auto active = started.get_future();
    const RequestHandler handler(kTestTokens, {});
    LogCapture logs;
    RunningServer fixture;
    TaskGate gate;
    fixture.use_workers([&, wait = gate.future](ConnectionId, const Request& request) {
        if (request.request_id == "0") {
            started.set_value();
            wait.wait();
        }
        return handler.handle(request).response;
    });
    // Queue the bytes and FIN before starting the loop, so EOF precedes task completion.
    auto client = connect_client(fixture.server.port());
    std::string bytes;
    for (int i = 0; i < request_count; ++i) {
        const nlohmann::json request{{"type", "request"}, {"request_id", std::to_string(i)},
                                     {"action", "ping"}};
        bytes += encode_frame(request.dump());
    }
    send_all(client.get(), bytes + tail);
    REQUIRE(::shutdown(client.get(), SHUT_WR) == 0);
    fixture.start();
    REQUIRE(active.wait_for(2s) == std::future_status::ready);

    auto other = connect_client(fixture.server.port());
    send_all(other.get(), encode_frame(R"({"type":"request","request_id":"other","action":"ping"})"));
    CHECK(nlohmann::json::parse(receive_frame(other.get()))["data"]["pong"] == true);
    gate.release();
    for (int i = 0; i < request_count; ++i) {
        const auto response = nlohmann::json::parse(receive_frame(client.get()));
        CHECK(response["request_id"] == std::to_string(i));
        CHECK(response["data"]["pong"] == true);
    }
    char byte;
    CHECK(::recv(client.get(), &byte, 1, 0) == 0);
    fixture.check();
    check_eof_logs(logs.output.str(), true);
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

TEST_CASE("Protocol errors allow later requests on the same TCP connection", "[server][protocol]") {
    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    fixture.server.set_message_handler([&](ConnectionId id, std::string payload) {
        fixture.server.send(id, handler.handle(payload).response.dump());
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(), encode_frame("{broken") +
                               encode_frame(R"({"type":"request","action":"ping"})") +
                               encode_frame(
                                   R"({"type":"request","request_id":"r1","action":"ping"})"));
    REQUIRE(::shutdown(client.get(), SHUT_WR) == 0);
    const auto malformed = nlohmann::json::parse(receive_frame(client.get()));
    const auto invalid = nlohmann::json::parse(receive_frame(client.get()));
    const auto success = nlohmann::json::parse(receive_frame(client.get()));
    CHECK(malformed["error"]["code"] == "invalid_json");
    CHECK(invalid["error"]["code"] == "invalid_request");
    CHECK(success["request_id"] == "r1");
    CHECK(success["ok"] == true);
    CHECK(success["data"]["pong"] == true);
    fixture.check();
}

TEST_CASE("Slow work leaves I/O responsive and preserves each connection's order",
          "[server][dispatcher]") {
    std::promise<void> slow_started;
    std::promise<void> next_started;
    auto slow = slow_started.get_future();
    auto next = next_started.get_future();
    RunningServer fixture;
    TaskGate gate;
    fixture.use_workers([&, wait = gate.future](ConnectionId, const Request& request) {
        if (request.action == "slow") {
            slow_started.set_value();
            wait.wait();
        } else if (request.action == "next") {
            next_started.set_value();
        }
        return make_response(request);
    });
    fixture.start();
    auto first = connect_client(fixture.server.port());
    auto second = connect_client(fixture.server.port());
    send_all(first.get(),
             encode_frame(R"({"type":"request","request_id":"a1","action":"slow"})") +
                 encode_frame(R"({"type":"request","request_id":"a2","action":"next"})"));
    REQUIRE(slow.wait_for(2s) == std::future_status::ready);
    CHECK(next.wait_for(0s) == std::future_status::timeout);

    send_all(second.get(),
             encode_frame(R"({"type":"request","request_id":"b1","action":"ping"})"));
    CHECK(nlohmann::json::parse(receive_frame(second.get()))["request_id"] == "b1");
    CHECK(next.wait_for(0s) == std::future_status::timeout);
    gate.release();
    CHECK(nlohmann::json::parse(receive_frame(first.get()))["request_id"] == "a1");
    CHECK(nlohmann::json::parse(receive_frame(first.get()))["request_id"] == "a2");
    fixture.check();
}

TEST_CASE("Half-closed clients receive every queued worker response", "[server][dispatcher]") {
    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    fixture.use_workers(
        [&](ConnectionId, const Request& request) { return handler.handle(request).response; });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    std::string frames;
    for (int i = 0; i < 20; ++i) {
        const nlohmann::json request{{"type", "request"}, {"request_id", std::to_string(i)},
                                     {"action", "ping"}};
        frames += encode_frame(request.dump());
    }
    send_all(client.get(), frames);
    REQUIRE(::shutdown(client.get(), SHUT_WR) == 0);
    for (int i = 0; i < 20; ++i) {
        const auto response = nlohmann::json::parse(receive_frame(client.get()));
        CHECK(response["request_id"] == std::to_string(i));
        CHECK(response["data"]["pong"] == true);
    }
    char byte;
    CHECK(::recv(client.get(), &byte, 1, 0) == 0);
    fixture.check();
}

TEST_CASE("Disconnect discards waiting requests and late worker completions",
          "[server][dispatcher]") {
    std::promise<ConnectionId> started;
    auto active = started.get_future();
    std::promise<void> disconnected;
    auto closed = disconnected.get_future();
    std::atomic<int> waiting_executed{0};
    std::atomic<ConnectionId> old_id{0};
    RunningServer fixture;
    TaskGate gate;
    fixture.use_workers([&, wait = gate.future](ConnectionId id, const Request& request) {
        if (request.action == "slow") {
            old_id = id;
            started.set_value(id);
            wait.wait();
        } else if (request.action == "waiting") {
            ++waiting_executed;
        }
        return make_response(request);
    });
    fixture.server.set_disconnect_handler([&](ConnectionId id) {
        fixture.dispatcher->disconnect(id);
        if (id == old_id) {
            disconnected.set_value();
        }
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(),
             encode_frame(R"({"type":"request","request_id":"old1","action":"slow"})") +
                 encode_frame(R"({"type":"request","request_id":"old2","action":"waiting"})"));
    REQUIRE(active.wait_for(2s) == std::future_status::ready);
    active.get();
    const linger reset{1, 0};
    REQUIRE(::setsockopt(client.get(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    client.reset();
    REQUIRE(closed.wait_for(2s) == std::future_status::ready);

    auto replacement = connect_client(fixture.server.port());
    gate.release();
    send_all(replacement.get(),
             encode_frame(R"({"type":"request","request_id":"fresh","action":"ping"})"));
    CHECK(nlohmann::json::parse(receive_frame(replacement.get()))["request_id"] == "fresh");
    fixture.check();
    CHECK(waiting_executed == 0);
}

TEST_CASE("Per-connection request limits close only the overloaded client",
          "[server][dispatcher][limits]") {
    std::promise<void> started;
    auto active = started.get_future();
    std::promise<void> disconnected;
    auto closed = disconnected.get_future();
    std::atomic<ConnectionId> old_id{0};
    std::atomic<int> waiting_executed{0};
    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    TaskGate gate;
    fixture.use_workers([&, wait = gate.future](ConnectionId id, const Request& request) {
        if (request.action == "slow") {
            old_id = id;
            started.set_value();
            wait.wait();
            return make_response(request);
        }
        if (request.action == "waiting") ++waiting_executed;
        return handler.handle(request).response;
    }, 2, 2, 64);
    fixture.server.set_disconnect_handler([&](ConnectionId id) {
        fixture.dispatcher->disconnect(id);
        if (id == old_id) disconnected.set_value();
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(), encode_frame(R"({"type":"request","request_id":"s","action":"slow"})"));
    REQUIRE(active.wait_for(2s) == std::future_status::ready);
    auto burst = encode_frame(R"({"type":"request","request_id":"w","action":"waiting"})");
    const auto extra = encode_frame(R"({"type":"request","request_id":"extra","action":"ping"})");
    for (int i = 0; i < 40; ++i) burst += extra;
    send_all(client.get(), burst);
    REQUIRE(closed.wait_for(2s) == std::future_status::ready);
    expect_closed(client.get());
    auto other = connect_client(fixture.server.port());
    send_all(other.get(), encode_frame(R"({"type":"request","request_id":"ok","action":"ping"})"));
    CHECK(nlohmann::json::parse(receive_frame(other.get()))["data"]["pong"] == true);
    gate.release();
    fixture.check();
    CHECK(waiting_executed == 0);
}

TEST_CASE("Disconnect releases waiting requests from the global budget",
          "[server][dispatcher][limits]") {
    std::promise<void> started;
    auto active = started.get_future();
    std::promise<void> enqueued;
    auto queued = enqueued.get_future();
    std::promise<void> disconnected;
    auto closed = disconnected.get_future();
    std::atomic<ConnectionId> old_id{0};
    std::atomic<int> waiting_executed{0};
    const std::string last = R"({"type":"request","request_id":"w2","action":"waiting"})";
    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    TaskGate gate;
    fixture.use_workers([&, wait = gate.future](ConnectionId id, const Request& request) {
        if (request.action == "slow") {
            old_id = id;
            started.set_value();
            wait.wait();
            return make_response(request);
        }
        if (request.action == "waiting") ++waiting_executed;
        return handler.handle(request).response;
    }, 2, 3, 3);
    fixture.server.set_message_handler([&](ConnectionId id, std::string payload) {
        const bool final_waiting_request = payload == last;
        fixture.dispatcher->enqueue(id, std::move(payload));
        if (final_waiting_request) enqueued.set_value();
    });
    fixture.server.set_disconnect_handler([&](ConnectionId id) {
        fixture.dispatcher->disconnect(id);
        if (id == old_id) disconnected.set_value();
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(), encode_frame(R"({"type":"request","request_id":"s","action":"slow"})"));
    REQUIRE(active.wait_for(2s) == std::future_status::ready);
    send_all(client.get(), encode_frame(R"({"type":"request","request_id":"w1","action":"waiting"})") +
                               encode_frame(last));
    REQUIRE(queued.wait_for(2s) == std::future_status::ready);
    const linger reset{1, 0};
    REQUIRE(::setsockopt(client.get(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    client.reset();
    REQUIRE(closed.wait_for(2s) == std::future_status::ready);
    auto other = connect_client(fixture.server.port());
    send_all(other.get(), encode_frame(R"({"type":"request","request_id":"ok","action":"ping"})"));
    CHECK(nlohmann::json::parse(receive_frame(other.get()))["data"]["pong"] == true);
    gate.release();
    fixture.check();
    CHECK(waiting_executed == 0);
}

TEST_CASE("Disconnected pool tasks retain their global slots until completion",
          "[server][dispatcher][limits]") {
    std::promise<void> started;
    auto active = started.get_future();
    std::promise<void> enqueued;
    auto queued = enqueued.get_future();
    std::promise<void> first_disconnected;
    auto first_closed = first_disconnected.get_future();
    std::promise<void> second_disconnected;
    auto second_closed = second_disconnected.get_future();
    std::promise<void> completion_applied;
    auto capacity = completion_applied.get_future();
    std::atomic<ConnectionId> first_id{0};
    ConnectionId second_id = 0;
    const std::string waiting = R"({"type":"request","request_id":"q","action":"queued"})";
    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    TaskGate gate;
    fixture.use_workers([&, wait = gate.future](ConnectionId id, const Request& request) {
        if (request.action == "slow") {
            first_id = id;
            started.set_value();
            wait.wait();
            return make_response(request);
        }
        if (request.action == "queued") {
            // With one worker, the first completion is posted before this task starts.
            fixture.server.post([&] { completion_applied.set_value(); });
            return make_response(request);
        }
        return handler.handle(request).response;
    }, 1, 4, 2);
    fixture.server.set_message_handler([&](ConnectionId id, std::string payload) {
        const bool pool_task = payload == waiting;
        if (pool_task) second_id = id;
        fixture.dispatcher->enqueue(id, std::move(payload));
        if (pool_task) enqueued.set_value();
    });
    fixture.server.set_disconnect_handler([&](ConnectionId id) {
        fixture.dispatcher->disconnect(id);
        if (id == first_id) first_disconnected.set_value();
        else if (id == second_id) second_disconnected.set_value();
    });
    fixture.start();
    auto first = connect_client(fixture.server.port());
    send_all(first.get(), encode_frame(R"({"type":"request","request_id":"s","action":"slow"})"));
    REQUIRE(active.wait_for(2s) == std::future_status::ready);
    auto second = connect_client(fixture.server.port());
    send_all(second.get(), encode_frame(waiting));
    REQUIRE(queued.wait_for(2s) == std::future_status::ready);
    const linger reset{1, 0};
    REQUIRE(::setsockopt(second.get(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    second.reset();
    REQUIRE(second_closed.wait_for(2s) == std::future_status::ready);
    REQUIRE(::setsockopt(first.get(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    first.reset();
    REQUIRE(first_closed.wait_for(2s) == std::future_status::ready);

    auto rejected = connect_client(fixture.server.port());
    send_all(rejected.get(), encode_frame(R"({"type":"request","request_id":"no","action":"ping"})"));
    expect_closed(rejected.get());
    gate.release();
    REQUIRE(capacity.wait_for(2s) == std::future_status::ready);
    auto replacement = connect_client(fixture.server.port());
    send_all(replacement.get(), encode_frame(R"({"type":"request","request_id":"ok","action":"ping"})"));
    const auto response = nlohmann::json::parse(receive_frame(replacement.get()));
    CHECK(response["request_id"] == "ok");
    CHECK(response["data"]["pong"] == true);
    fixture.check();
}

TEST_CASE("Error completions release request capacity for later requests",
          "[server][dispatcher][limits]") {
    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    fixture.use_workers([&](ConnectionId, const Request& request) {
        if (request.action == "fail") throw std::runtime_error("Expected handler failure");
        return handler.handle(request).response;
    }, 1, 1, 1);
    fixture.start();
    auto client = connect_client(fixture.server.port());
    for (const auto* payload : {"{broken",
                                R"({"type":"request","request_id":"f","action":"fail"})",
                                R"({"type":"request","request_id":"u","action":"missing"})"}) {
        send_all(client.get(), encode_frame(payload));
        CHECK(nlohmann::json::parse(receive_frame(client.get()))["ok"] == false);
    }
    send_all(client.get(), encode_frame(R"({"type":"request","request_id":"ok","action":"ping"})"));
    CHECK(nlohmann::json::parse(receive_frame(client.get()))["data"]["pong"] == true);
    fixture.check();
}

TEST_CASE("A failed worker request does not stall the next request", "[server][dispatcher]") {
    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    fixture.use_workers([&](ConnectionId, const Request& request) {
        if (request.action == "fail") {
            throw std::runtime_error("Expected handler failure");
        }
        return handler.handle(request).response;
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(),
             encode_frame(R"({"type":"request","request_id":"bad","action":"fail"})") +
                 encode_frame("{broken") +
                 encode_frame(R"({"type":"request","request_id":"good","action":"ping"})"));
    const auto failure = nlohmann::json::parse(receive_frame(client.get()));
    CHECK(failure["request_id"] == "bad");
    CHECK(failure["error"]["code"] == "internal_error");
    const auto malformed = nlohmann::json::parse(receive_frame(client.get()));
    CHECK(malformed["error"]["code"] == "invalid_json");
    const auto success = nlohmann::json::parse(receive_frame(client.get()));
    CHECK(success["request_id"] == "good");
    CHECK(success["ok"] == true);
    fixture.check();
}

TEST_CASE("Excessive JSON nesting does not stall later worker requests",
          "[server][dispatcher][protocol]") {
    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    fixture.use_workers([&](ConnectionId, const Request& request) {
        return handler.handle(request).response;
    }, 1);
    fixture.start();
    auto client = connect_client(fixture.server.port());
    const auto deep =
        R"({"type":"request","request_id":"deep","action":"ping","data":{"nested":)" +
        std::string(30000, '[') + "0" + std::string(30000, ']') + "}}";
    send_all(client.get(), encode_frame(deep) +
        encode_frame(R"({"type":"request","request_id":"next","action":"ping"})"));
    REQUIRE(::shutdown(client.get(), SHUT_WR) == 0);

    const auto error = nlohmann::json::parse(receive_frame(client.get()));
    CHECK(error["ok"] == false);
    CHECK(error["error"]["code"] == "invalid_request");
    CHECK(error["request_id"].is_null());
    CHECK(error["action"].is_null());
    const auto response = nlohmann::json::parse(receive_frame(client.get()));
    CHECK(response["request_id"] == "next");
    CHECK(response["ok"] == true);
    CHECK(response["data"]["pong"] == true);
    expect_closed(client.get());
    fixture.check();
}

TEST_CASE("Oversized worker responses do not interrupt request processing", "[server][dispatcher]") {
    std::string text;
    bool error_response = false;
    bool throw_error = false;
    SECTION("Large success response") { text.assign(kMaxFrameSize, 'x'); }
    SECTION("Escaping makes the serialized response too large") {
        text.assign(kMaxFrameSize / 2, '\n');
    }
    SECTION("Large application error response") {
        text.assign(kMaxFrameSize, 'x');
        error_response = true;
    }
    SECTION("Large ProtocolError message") {
        text.assign(kMaxFrameSize, 'x');
        throw_error = true;
    }
    SECTION("Escaping makes a ProtocolError response too large") {
        text.assign(kMaxFrameSize / 2, '\n');
        throw_error = true;
    }

    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    fixture.use_workers([&](ConnectionId, const Request& request) {
        if (request.action == "large") {
            if (throw_error) throw ProtocolError("test_error", text, request);
            if (error_response) return make_error_response(request, "test_error", text);
            return make_response(request, {{"text", text}});
        }
        return handler.handle(request).response;
    });
    fixture.start();
    auto first = connect_client(fixture.server.port());
    auto second = connect_client(fixture.server.port());
    send_all(first.get(),
             encode_frame(R"({"type":"request","request_id":"large-1","action":"large"})") +
                 encode_frame(R"({"type":"request","request_id":"next","action":"ping"})"));
    REQUIRE(::shutdown(first.get(), SHUT_WR) == 0);
    const auto failure = nlohmann::json::parse(receive_frame(first.get()));
    CHECK(failure["request_id"] == "large-1");
    CHECK(failure["action"] == "large");
    CHECK(failure["ok"] == false);
    CHECK(failure["error"]["code"] == "response_too_large");
    CHECK(failure["error"]["message"] == "Response exceeds the 64 KiB limit");
    const auto next = nlohmann::json::parse(receive_frame(first.get()));
    CHECK(next["request_id"] == "next");
    CHECK(next["data"]["pong"] == true);
    char byte;
    CHECK(::recv(first.get(), &byte, 1, 0) == 0);

    send_all(second.get(),
             encode_frame(R"({"type":"request","request_id":"other","action":"ping"})"));
    const auto other = nlohmann::json::parse(receive_frame(second.get()));
    CHECK(other["request_id"] == "other");
    CHECK(other["data"]["pong"] == true);
    fixture.check();
}

TEST_CASE("Worker responses respect the exact serialized byte limit", "[server][dispatcher]") {
    std::size_t extra_byte = 0;
    bool throw_error = false;
    SECTION("A response of exactly 64 KiB is sent") {}
    SECTION("A response one byte over the limit is replaced") { extra_byte = 1; }
    SECTION("A ProtocolError response of exactly 64 KiB is sent") { throw_error = true; }
    SECTION("A ProtocolError response one byte over the limit is replaced") {
        throw_error = true;
        extra_byte = 1;
    }

    RunningServer fixture;
    fixture.use_workers([extra_byte, throw_error](ConnectionId, const Request& request) {
        auto response = throw_error ? make_error_response(request, "test_error", "")
                                    : make_response(request, {{"text", ""}});
        const auto envelope_size = response.dump().size();
        const std::string text(kMaxFrameSize - envelope_size + extra_byte, 'x');
        if (throw_error) throw ProtocolError("test_error", text, request);
        response["data"]["text"] = text;
        return response;
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(),
             encode_frame(R"({"type":"request","request_id":"boundary","action":"large"})"));
    const auto payload = receive_frame(client.get());
    const auto response = nlohmann::json::parse(payload);
    CHECK(response["request_id"] == "boundary");
    CHECK(response["action"] == "large");
    if (extra_byte == 0) {
        CHECK(payload.size() == kMaxFrameSize);
        CHECK(response["ok"] == !throw_error);
        if (throw_error) CHECK(response["error"]["code"] == "test_error");
    } else {
        CHECK(payload.size() < kMaxFrameSize);
        CHECK(response["ok"] == false);
        CHECK(response["error"]["code"] == "response_too_large");
    }
    fixture.check();
}

TEST_CASE("Response serialization failures do not stall later requests", "[server][dispatcher]") {
    bool throw_error = false;
    SECTION("Invalid UTF-8 in a returned response") {}
    SECTION("Invalid UTF-8 in a ProtocolError message") { throw_error = true; }

    const RequestHandler handler(kTestTokens, {});
    RunningServer fixture;
    fixture.use_workers([&](ConnectionId, const Request& request) {
        if (request.action == "fail") {
            const std::string text(1, '\xff');
            if (throw_error) throw ProtocolError("test_error", text, request);
            return make_response(request, {{"text", text}});
        }
        return handler.handle(request).response;
    }, 1, 1, 1);
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(),
             encode_frame(R"({"type":"request","request_id":"bad","action":"fail"})"));
    const auto failure = nlohmann::json::parse(receive_frame(client.get()));
    CHECK(failure["request_id"] == "bad");
    CHECK(failure["action"] == "fail");
    CHECK(failure["ok"] == false);
    CHECK(failure["error"]["code"] == "internal_error");
    CHECK(failure["error"]["message"] == "Request processing failed");

    send_all(client.get(),
             encode_frame(R"({"type":"request","request_id":"next","action":"ping"})"));
    const auto response = nlohmann::json::parse(receive_frame(client.get()));
    CHECK(response["request_id"] == "next");
    CHECK(response["data"]["pong"] == true);
    fixture.check();
}

TEST_CASE("Stopping the Reactor before workers finish is safe", "[server][dispatcher]") {
    std::promise<void> started;
    auto active = started.get_future();
    std::atomic<int> finished{0};
    std::atomic<bool> completion_ran{false};
    RunningServer fixture;
    TaskGate gate;
    fixture.use_workers([&, wait = gate.future](ConnectionId, const Request& request) {
        started.set_value();
        wait.wait();
        ++finished;
        return make_response(request);
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(),
             encode_frame(R"({"type":"request","request_id":"r1","action":"slow"})"));
    REQUIRE(active.wait_for(2s) == std::future_status::ready);
    fixture.stop_loop();
    fixture.server.post([&] { completion_ran = true; });
    char byte;
    CHECK(::recv(client.get(), &byte, 1, 0) == 0);
    gate.release();
    fixture.check();
    CHECK(finished == 1);
    CHECK_FALSE(completion_ran);
}

TEST_CASE("Authentication is applied before pipelined requests take their session snapshot",
          "[server][dispatcher][session]") {
    RunningServer fixture;
    fixture.use_session_workers([](ConnectionId, const Request& request, std::optional<Session> session) {
        if (request.action == "authenticate") {
            return RequestResult{make_response(request),
                Session{7, std::chrono::system_clock::now() + 1h}};
        }
        if (request.action == "failed_auth") {
            return RequestResult{make_error_response(request, "invalid_token", "Invalid token")};
        }
        return RequestResult{make_response(request, {{"user_id", session ? session->user_id : 0}})};
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    std::string bytes;
    for (const auto* action : {"probe", "authenticate", "probe", "failed_auth", "probe"}) {
        bytes += encode_frame(nlohmann::json{{"type", "request"}, {"request_id", action}, {"action", action}}.dump());
    }
    send_all(client.get(), bytes);
    CHECK(nlohmann::json::parse(receive_frame(client.get()))["data"]["user_id"] == 0);
    CHECK(nlohmann::json::parse(receive_frame(client.get()))["ok"] == true);
    CHECK(nlohmann::json::parse(receive_frame(client.get()))["data"]["user_id"] == 7);
    CHECK(nlohmann::json::parse(receive_frame(client.get()))["error"]["code"] == "invalid_token");
    CHECK(nlohmann::json::parse(receive_frame(client.get()))["data"]["user_id"] == 7);
    fixture.check();
}

TEST_CASE("A new authenticated connection replaces the old connection without losing its binding",
          "[server][dispatcher][session]") {
    RunningServer fixture;
    fixture.use_session_workers([](ConnectionId, const Request& request, std::optional<Session>) {
        return RequestResult{make_response(request), Session{7, std::chrono::system_clock::now() + 1h}};
    });
    fixture.start();
    const auto authenticate = encode_frame(R"({"type":"request","request_id":"a","action":"authenticate"})");
    auto old = connect_client(fixture.server.port());
    send_all(old.get(), authenticate);
    REQUIRE(nlohmann::json::parse(receive_frame(old.get()))["ok"] == true);
    auto current = connect_client(fixture.server.port());
    send_all(current.get(), authenticate);
    REQUIRE(nlohmann::json::parse(receive_frame(current.get()))["ok"] == true);
    expect_closed(old.get());
    // Refreshing the same connection must not close itself.
    send_all(current.get(), authenticate);
    CHECK(nlohmann::json::parse(receive_frame(current.get()))["ok"] == true);
    std::promise<bool> bound;
    auto ready = bound.get_future();
    fixture.server.post([&] { bound.set_value(fixture.dispatcher->sessions().connection_for(7).has_value()); });
    REQUIRE(ready.wait_for(2s) == std::future_status::ready);
    CHECK(ready.get());
    fixture.check();
}

TEST_CASE("A disconnected authentication task cannot replace an online session",
          "[server][dispatcher][session]") {
    std::promise<ConnectionId> started;
    auto active = started.get_future();
    std::promise<void> disconnected;
    auto closed = disconnected.get_future();
    std::atomic<ConnectionId> delayed_id{0};
    RunningServer fixture;
    TaskGate gate;
    fixture.use_session_workers([&, wait = gate.future](ConnectionId id, const Request& request,
                                                       std::optional<Session> session) {
        if (request.request_id == "delayed") {
            delayed_id = id;
            started.set_value(id);
            wait.wait();
        }
        if (request.action == "authenticate") {
            return RequestResult{make_response(request), Session{7, std::chrono::system_clock::now() + 1h}};
        }
        return RequestResult{make_response(request, {{"user_id", session ? session->user_id : 0}})};
    }, 1);
    fixture.server.set_disconnect_handler([&](ConnectionId id) {
        fixture.dispatcher->disconnect(id);
        if (id == delayed_id) disconnected.set_value();
    });
    fixture.start();
    auto current = connect_client(fixture.server.port());
    send_all(current.get(), encode_frame(R"({"type":"request","request_id":"current","action":"authenticate"})"));
    REQUIRE(nlohmann::json::parse(receive_frame(current.get()))["ok"] == true);
    auto delayed = connect_client(fixture.server.port());
    send_all(delayed.get(), encode_frame(R"({"type":"request","request_id":"delayed","action":"authenticate"})"));
    REQUIRE(active.wait_for(2s) == std::future_status::ready);
    const linger reset{1, 0};
    REQUIRE(::setsockopt(delayed.get(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    delayed.reset();
    REQUIRE(closed.wait_for(2s) == std::future_status::ready);
    gate.release();
    send_all(current.get(), encode_frame(R"({"type":"request","request_id":"probe","action":"probe"})"));
    CHECK(nlohmann::json::parse(receive_frame(current.get()))["data"]["user_id"] == 7);
    fixture.check();
}

TEST_CASE("Expired or oversized authentication results cannot establish sessions",
          "[server][dispatcher][session]") {
    bool oversized = false;
    SECTION("Token expires before the Reactor applies the result") {}
    SECTION("Response serialization rejects the authentication result") { oversized = true; }
    RunningServer fixture;
    fixture.use_session_workers([oversized](ConnectionId, const Request& request, std::optional<Session> session) {
        if (request.action == "authenticate") {
            return RequestResult{make_response(request, {{"text", std::string(oversized ? kMaxFrameSize : 0, 'x')}}),
                Session{7, std::chrono::system_clock::now() + (oversized ? 1h : -1h)}};
        }
        return RequestResult{make_response(request, {{"authenticated", session.has_value()}})};
    });
    fixture.start();
    auto client = connect_client(fixture.server.port());
    send_all(client.get(), encode_frame(R"({"type":"request","request_id":"a","action":"authenticate"})") +
                          encode_frame(R"({"type":"request","request_id":"p","action":"probe"})"));
    const auto error = nlohmann::json::parse(receive_frame(client.get()));
    CHECK(error["request_id"] == "a");
    CHECK(error["error"]["code"] == (oversized ? "response_too_large" : "invalid_token"));
    CHECK(nlohmann::json::parse(receive_frame(client.get()))["data"]["authenticated"] == false);
    fixture.check();
}
