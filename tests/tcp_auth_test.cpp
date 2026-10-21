#include "database_fixture.h"
#include "http/http_server.h"
#include "request_dispatcher.h"

#include <algorithm>
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
          }, 2, 32, 1024, [this](std::uint64_t user) { return handler.pending_messages(user); }),
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
    first.send("unknown");
    CHECK(first.receive()["error"]["code"] == "authentication_required");
    first.send("authenticate", {{"token", "incorrect-token"}});
    CHECK(first.receive()["error"]["code"] == "invalid_token");
    // Queue both requests before receiving either response.
    first.send("authenticate", {{"token", token}, {"user_id", "999999"}});
    first.send("unknown");
    CHECK(first.receive()["data"]["user_id"] == user_id);
    CHECK(first.receive()["error"]["code"] == "unknown_action");

    // Invalid refreshes keep the existing identity.
    first.send("authenticate", {{"token", "incorrect-token"}});
    CHECK(first.receive()["error"]["code"] == "invalid_token");
    first.send("unknown");
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
    second.send("unknown");
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

namespace {
void authenticate(Peer& peer, const RunningChat& running, std::uint64_t user, bool empty_inbox = true) {
    peer.send("authenticate", {{"token", running.tokens.issue(user)}});
    REQUIRE(peer.receive()["data"]["user_id"] == std::to_string(user));
    if (empty_inbox) {
        peer.send("ping");
        REQUIRE(peer.receive()["data"]["pong"] == true);
    }
}

json message_data(const std::string& client_id, const std::vector<std::uint64_t>& recipients,
                  const std::string& text) {
    auto ids = json::array();
    for (const auto id : recipients) ids.push_back(std::to_string(id));
    return {{"client_message_id", client_id}, {"recipients", std::move(ids)}, {"text", text}};
}
} // namespace

TEST_CASE("Authenticated peers exchange persisted private messages", "[database][tcp][message]") {
    DatabaseFixture fixture;
    const auto alice = fixture.store.create_user("alice", "unused-test-hash");
    const auto bob = fixture.store.create_user("bob", "unused-test-hash");
    RunningChat running;
    Peer first(running.server.port());
    Peer second(running.server.port());
    authenticate(first, running, alice);
    authenticate(second, running, bob);
    auto data = message_data("alice-1", {bob}, u8"Hello \U0001f642");
    data["sender_id"] = std::to_string(bob);
    first.send("send_message", data);
    const auto response = first.receive();
    REQUIRE(response["ok"] == true);
    const auto notice = second.receive();
    CHECK(notice["type"] == "notification");
    CHECK(notice["action"] == "message");
    CHECK_FALSE(notice.contains("request_id"));
    CHECK(notice["data"] == json{{"message_id", response["data"]["message_id"]},
                                  {"sender_id", std::to_string(alice)},
                                  {"client_message_id", "alice-1"}, {"text", data["text"]}});
    // This independent connection must see the committed record after delivery.
    const auto stored = fixture.store.pending_messages(bob);
    REQUIRE(stored.size() == 1);
    CHECK(std::to_string(stored[0].id) == response["data"]["message_id"]);
    CHECK(stored[0].sender_id == alice);
    CHECK(stored[0].text == data["text"]);
    CHECK_FALSE(stored[0].group_id);

    second.send("send_message", message_data("bob-1", {alice}, "Hi Alice"));
    REQUIRE(second.receive()["ok"] == true);
    const auto reply = first.receive();
    CHECK(reply["data"]["sender_id"] == std::to_string(bob));
    CHECK(reply["data"]["text"] == "Hi Alice");
    CHECK(fixture.count("messages") == 2);
    CHECK(fixture.count("message_deliveries") == 2);
    running.check();
}

TEST_CASE("Multiple recipients share one message including self and offline users",
          "[database][tcp][message]") {
    DatabaseFixture fixture;
    const auto alice = fixture.store.create_user("alice", "unused-test-hash");
    const auto bob = fixture.store.create_user("bob", "unused-test-hash");
    const auto offline = fixture.store.create_user("offline", "unused-test-hash");
    RunningChat running;
    Peer first(running.server.port());
    Peer second(running.server.port());
    authenticate(first, running, alice);
    authenticate(second, running, bob);
    first.send("send_message", message_data("multi-1", {bob, alice, offline, bob}, "Hello everyone"));
    const auto response = first.receive();
    REQUIRE(response["type"] == "response");
    REQUIRE(response["ok"] == true);
    const auto self = first.receive();
    const auto other = second.receive();
    CHECK(self == other);
    CHECK(other["data"]["message_id"] == response["data"]["message_id"]);
    CHECK(fixture.count("messages") == 1);
    CHECK(fixture.count("message_deliveries") == 3);
    CHECK(fixture.store.pending_messages(offline).size() == 1);
    second.send("ping");
    CHECK(second.receive()["data"]["pong"] == true);
    running.check();
}

TEST_CASE("A message can target 64 distinct recipients", "[database][tcp][message]") {
    DatabaseFixture fixture;
    const auto sender = fixture.store.create_user("sender", "unused-test-hash");
    std::vector<std::uint64_t> recipients;
    for (int i = 0; i < 64; ++i) {
        recipients.push_back(fixture.store.create_user("recipient-" + std::to_string(i),
                                                      "unused-test-hash"));
    }
    std::reverse(recipients.begin(), recipients.end());
    RunningChat running;
    Peer peer(running.server.port());
    authenticate(peer, running, sender);
    peer.send("send_message", message_data("multi-64", recipients, "Hello everyone"));
    const auto response = peer.receive();
    REQUIRE(response["ok"] == true);
    CHECK(fixture.count("messages") == 1);
    CHECK(fixture.count("message_deliveries") == 64);
    for (const auto recipient : recipients) {
        const auto pending = fixture.store.pending_messages(recipient);
        REQUIRE(pending.size() == 1);
        CHECK(std::to_string(pending[0].id) == response["data"]["message_id"]);
    }
    running.check();
}

TEST_CASE("Missing recipients reject the entire message without notifying valid recipients",
          "[database][tcp][message]") {
    DatabaseFixture fixture;
    const auto alice = fixture.store.create_user("alice", "unused-test-hash");
    const auto bob = fixture.store.create_user("bob", "unused-test-hash");
    RunningChat running;
    Peer first(running.server.port());
    Peer second(running.server.port());
    authenticate(first, running, alice);
    authenticate(second, running, bob);
    first.send("send_message", message_data("invalid-1", {bob, 18446744073709551615ULL}, "Hello"));
    CHECK(first.receive()["error"]["code"] == "invalid_recipient");
    CHECK(fixture.count("messages") == 0);
    CHECK(fixture.count("message_deliveries") == 0);
    second.send("ping");
    CHECK(second.receive()["data"]["pong"] == true);
    running.check();
}

TEST_CASE("Message retries reuse their ID and reject changed text or recipients",
          "[database][tcp][message]") {
    DatabaseFixture fixture;
    const auto alice = fixture.store.create_user("alice", "unused-test-hash");
    const auto bob = fixture.store.create_user("bob", "unused-test-hash");
    const auto other = fixture.store.create_user("other", "unused-test-hash");
    RunningChat running;
    Peer first(running.server.port());
    Peer second(running.server.port());
    authenticate(first, running, alice);
    authenticate(second, running, bob);
    const auto data = message_data("retry-1", {bob}, "Hello");
    first.send("send_message", data);
    const auto original = first.receive();
    REQUIRE(original["ok"] == true);
    const auto notice = second.receive();
    first.send("send_message", message_data("retry-1", {bob, bob}, "Hello"));
    CHECK(first.receive()["data"]["message_id"] == original["data"]["message_id"]);
    CHECK(second.receive() == notice);
    for (const auto& changed : {message_data("retry-1", {bob}, "Changed text"),
                                message_data("retry-1", {other}, "Hello")}) {
        first.send("send_message", changed);
        CHECK(first.receive()["error"]["code"] == "message_conflict");
    }
    CHECK(fixture.count("messages") == 1);
    CHECK(fixture.count("message_deliveries") == 1);
    second.send("ping");
    CHECK(second.receive()["data"]["pong"] == true);
    running.check();
}

TEST_CASE("Message byte limits remain safe after JSON escaping", "[database][tcp][message]") {
    DatabaseFixture fixture;
    const auto alice = fixture.store.create_user("alice", "unused-test-hash");
    const auto bob = fixture.store.create_user("bob", "unused-test-hash");
    RunningChat running;
    Peer first(running.server.port());
    Peer second(running.server.port());
    authenticate(first, running, alice);
    authenticate(second, running, bob);
    const std::string client_id(64, '\1');
    const std::string text(4096, '\0');
    first.send("send_message", message_data(client_id, std::vector<std::uint64_t>(64, bob), text));
    REQUIRE(first.receive()["ok"] == true);
    const auto notice = second.receive();
    CHECK(notice["data"]["client_message_id"] == client_id);
    CHECK(notice["data"]["text"] == text);
    CHECK(notice.dump().size() <= kMaxFrameSize);
    REQUIRE(fixture.store.pending_messages(bob).size() == 1);
    CHECK(fixture.store.pending_messages(bob)[0].text == text);
    CHECK(fixture.count("message_deliveries") == 1);
    running.check();
}

TEST_CASE("Offline deliveries replay until acknowledged and acknowledgments belong to the recipient",
          "[database][tcp][message][ack][replay]") {
    DatabaseFixture fixture;
    const auto alice = fixture.store.create_user("alice", "unused-test-hash");
    const auto bob = fixture.store.create_user("bob", "unused-test-hash");
    const auto other = fixture.store.create_user("other", "unused-test-hash");
    RunningChat running;
    Peer sender(running.server.port());
    authenticate(sender, running, alice);
    sender.send("send_message", message_data("offline-1", {bob}, "Saved while offline"));
    const auto sent = sender.receive();
    REQUIRE(sent["ok"] == true);
    const auto message = sent["data"]["message_id"];
    json original;
    {
        Peer recipient(running.server.port());
        authenticate(recipient, running, bob, false);
        original = recipient.receive();
        CHECK(original["data"]["message_id"] == message);
        CHECK(original["data"]["text"] == "Saved while offline");
    }
    Peer recipient(running.server.port());
    authenticate(recipient, running, bob, false);
    CHECK(recipient.receive() == original);
    Peer outsider(running.server.port());
    authenticate(outsider, running, other);
    outsider.send("ack_message", {{"message_id", message}, {"recipient_id", std::to_string(bob)}});
    CHECK(outsider.receive()["error"]["code"] == "invalid_message");
    REQUIRE(fixture.store.pending_messages(bob).size() == 1);
    for (int i = 0; i < 2; ++i) {
        recipient.send("ack_message", {{"message_id", message}});
        const auto response = recipient.receive();
        CHECK(response["ok"] == true);
        CHECK(response["data"]["message_id"] == message);
    }
    CHECK(fixture.store.pending_messages(bob).empty());
    recipient.send("ack_message", {{"message_id", "18446744073709551615"}});
    CHECK(recipient.receive()["error"]["code"] == "invalid_message");
    Peer reconnected(running.server.port());
    authenticate(reconnected, running, bob);
    sender.send("send_message", message_data("offline-1", {bob}, "Saved while offline"));
    CHECK(sender.receive()["data"]["message_id"] == message);
    CHECK(reconnected.receive() == original);
    CHECK(fixture.store.pending_messages(bob).empty());
    running.check();
}

TEST_CASE("Replay advances only after its batch is acknowledged and respects count and byte limits",
          "[database][tcp][message][replay]") {
    std::string text = "Small message";
    SECTION("Count limit") {}
    SECTION("Escaped bytes limit") { text.assign(4096, '\0'); }
    DatabaseFixture fixture;
    const auto sender = fixture.store.create_user("sender", "unused-test-hash");
    const auto recipient = fixture.store.create_user("recipient", "unused-test-hash");
    for (std::size_t i = 0; i < kReplayBatchCount + 3; ++i) {
        fixture.store.save_direct_message(sender, "batch-" + std::to_string(i), text, {recipient});
    }
    RunningChat running;
    Peer peer(running.server.port());
    authenticate(peer, running, recipient, false);
    std::size_t total = 0;
    while (total < kReplayBatchCount + 3) {
        const auto batch = running.handler.pending_messages(recipient);
        REQUIRE_FALSE(batch.empty());
        CHECK(batch.size() <= kReplayBatchCount);
        if (total == 0) CHECK(batch.size() == (text.front() == '\0' ? 5 : kReplayBatchCount));
        std::size_t bytes = 0;
        for (const auto& delivery : batch) {
            const auto notice = peer.receive();
            CHECK(notice["data"]["message_id"] == std::to_string(delivery.message_id));
            CHECK(notice["data"]["text"] == text);
            bytes += notice.dump().size() + 4;
        }
        CHECK(bytes <= kReplayBatchBytes);
        peer.send("ping");
        CHECK(peer.receive()["data"]["pong"] == true);
        for (std::size_t i = 0; i < batch.size(); ++i) {
            peer.send("ack_message", {{"message_id", std::to_string(batch[i].message_id)}});
            CHECK(peer.receive()["ok"] == true);
            if (i == 0 && batch.size() > 1) {
                peer.send("ack_message", {{"message_id", std::to_string(batch[i].message_id)}});
                CHECK(peer.receive()["ok"] == true);
            }
            if (i + 1 < batch.size()) {
                peer.send("ping");
                CHECK(peer.receive()["data"]["pong"] == true);
            }
        }
        total += batch.size();
    }
    CHECK(fixture.store.pending_messages(recipient).empty());
    peer.send("ping");
    CHECK(peer.receive()["data"]["pong"] == true);
    running.check();
}
