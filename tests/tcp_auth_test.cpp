#include "database_fixture.h"
#include "http/http_server.h"
#include "request_dispatcher.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

using namespace chatroom;
using nlohmann::json;

namespace {
class RunningChat {
public:
    TokenService tokens{std::string(32, 'k'), 60};
    AuthService auth{tokens};
    RequestHandler handler;
    TcpServer server{0};
    RequestDispatcher dispatcher;
    HttpServer http;

    explicit RunningChat(DatabaseConfig database = test_config())
        : handler(tokens, database),
          dispatcher(server, [this](ConnectionId, const Request& request, std::optional<Session> session) {
              return handler.handle(request, session);
          }, 2),
          http(auth, database, "127.0.0.1", HttpConfig{0, 2}) {
        server.set_message_handler([this](ConnectionId id, std::string payload) {
            dispatcher.enqueue(id, std::move(payload));
        });
        server.set_disconnect_handler([this](ConnectionId id) { dispatcher.disconnect(id); });
        http.start([this] { server.stop(); });
        loop_ = std::thread([this] {
            try { server.run(); }
            catch (...) { failure_ = std::current_exception(); }
        });
    }

    ~RunningChat() { stop(); }

    void check() {
        stop();
        http.check();
        if (failure_) std::rethrow_exception(failure_);
    }

private:
    void stop() {
        server.stop();
        if (loop_.joinable()) loop_.join();
        http.stop();
    }

    std::thread loop_;
    std::exception_ptr failure_;
};

// A small framed TCP peer for tests that also build with BUILD_CLIENT=OFF.
class Peer {
public:
    UniqueFd socket{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};

    explicit Peer(std::uint16_t port) {
        REQUIRE(socket.get() >= 0);
        const timeval timeout{3, 0};
        REQUIRE(::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        REQUIRE(::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        REQUIRE(::connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    }

    void send(const std::string& action, json data = json::object()) {
        const auto frame = encode_frame(json{{"type", "request"}, {"request_id", action},
                                             {"action", action}, {"data", std::move(data)}}.dump());
        std::string_view remaining(frame);
        while (!remaining.empty()) {
            const auto count = ::send(socket.get(), remaining.data(), remaining.size(), MSG_NOSIGNAL);
            if (count < 0 && errno == EINTR) continue;
            REQUIRE(count > 0);
            remaining.remove_prefix(static_cast<std::size_t>(count));
        }
    }

    json receive() {
        const auto header = read(4);
        std::uint32_t size;
        std::memcpy(&size, header.data(), sizeof(size));
        size = ntohl(size);
        REQUIRE(size <= kMaxFrameSize);
        return json::parse(read(size));
    }

private:
    std::string read(std::size_t size) {
        std::string bytes(size, '\0');
        std::size_t received = 0;
        while (received < size) {
            const auto count = ::recv(socket.get(), bytes.data() + received, size - received, 0);
            if (count < 0 && errno == EINTR) continue;
            REQUIRE(count > 0);
            received += static_cast<std::size_t>(count);
        }
        return bytes;
    }
};
} // namespace

TEST_CASE("HTTP tokens authenticate TCP sessions and successful login replaces the previous peer",
          "[database][http][tcp][auth]") {
    DatabaseFixture fixture;
    std::ostringstream logs;
    struct RestoreLog {
        std::streambuf* previous;
        ~RestoreLog() { std::clog.rdbuf(previous); }
    } restore{std::clog.rdbuf(logs.rdbuf())};
    RunningChat running;
    httplib::Client http("127.0.0.1", running.http.port());
    const auto body = json{{"username", "alice"}, {"password", "correct-password"}}.dump();
    const auto registration = http.Post("/api/users", body, "application/json");
    REQUIRE(registration);
    REQUIRE(registration->status == 201);
    const auto user_id = json::parse(registration->body)["id"];
    const auto login = http.Post("/api/auth/token", body, "application/json");
    REQUIRE(login);
    REQUIRE(login->status == 200);
    const auto token = json::parse(login->body)["access_token"];

    Peer first(running.server.port());
    first.send("ping");
    CHECK(first.receive()["data"]["pong"] == true);
    first.send("send_message");
    CHECK(first.receive()["error"]["code"] == "authentication_required");
    first.send("authenticate", {{"token", "incorrect-token"}});
    CHECK(first.receive()["error"]["code"] == "invalid_token");
    // Queue both requests before receiving either response.
    first.send("authenticate", {{"token", token}, {"user_id", "999999"}});
    first.send("send_message");
    CHECK(first.receive()["data"]["user_id"] == user_id);
    CHECK(first.receive()["error"]["code"] == "unknown_action");

    // Invalid refreshes keep the existing identity.
    first.send("authenticate", {{"token", "incorrect-token"}});
    CHECK(first.receive()["error"]["code"] == "invalid_token");
    first.send("send_message");
    CHECK(first.receive()["error"]["code"] == "unknown_action");
    first.send("authenticate", {{"token", token}});
    CHECK(first.receive()["data"]["user_id"] == user_id);

    const auto bob = fixture.store.create_user("bob", "unused-test-hash");
    first.send("authenticate", {{"token", running.tokens.issue(bob)}});
    CHECK(first.receive()["error"]["code"] == "already_authenticated");
    Peer second(running.server.port());
    second.send("authenticate", {{"token", token}});
    CHECK(second.receive()["data"]["user_id"] == user_id);
    char byte;
    const auto count = ::recv(first.socket.get(), &byte, 1, 0);
    CHECK((count == 0 || (count < 0 && errno == ECONNRESET)));
    second.send("send_message");
    CHECK(second.receive()["error"]["code"] == "unknown_action");
    running.check();
    CHECK_FALSE(running.dispatcher.sessions().connection_for(std::stoull(user_id.get<std::string>())));
    CHECK(logs.str().find(token.get<std::string>()) == std::string::npos);
    CHECK(logs.str().find("correct-password") == std::string::npos);
    CHECK(logs.str().find("incorrect-token") == std::string::npos);
}

TEST_CASE("A signed token for a missing user cannot authenticate TCP", "[database][tcp][auth]") {
    DatabaseFixture fixture;
    const auto user = fixture.store.create_user("removed-user", "unused-test-hash");
    fixture.database.execute("DELETE FROM users WHERE id = ?", {user});
    RunningChat running;
    Peer peer(running.server.port());
    peer.send("authenticate", {{"token", running.tokens.issue(user)}});
    const auto response = peer.receive();
    CHECK(response["request_id"] == "authenticate");
    CHECK(response["error"]["code"] == "invalid_token");
    peer.send("send_message");
    CHECK(peer.receive()["error"]["code"] == "authentication_required");
    running.check();
}

TEST_CASE("TCP database errors do not bind sessions or stop the Reactor", "[database][tcp][auth]") {
    DatabaseFixture fixture;
    const auto user = fixture.store.create_user("alice", "unused-test-hash");
    auto config = test_config();
    config.password = "deliberately-incorrect-tcp-test-password";
    RunningChat running(config);
    Peer peer(running.server.port());
    peer.send("authenticate", {{"token", running.tokens.issue(user)}});
    const auto response = peer.receive();
    CHECK(response["error"]["code"] == "internal_error");
    CHECK(response.dump().find(config.password) == std::string::npos);
    peer.send("send_message");
    CHECK(peer.receive()["error"]["code"] == "authentication_required");
    peer.send("ping");
    CHECK(peer.receive()["data"]["pong"] == true);
    running.check();
}
