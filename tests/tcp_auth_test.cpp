#include "database_fixture.h"
#include "http/http_server.h"
#include "request_dispatcher.h"
#include "temp_directory.h"

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

    explicit RunningChat(DatabaseConfig database = test_config(), FileConfig files = {})
        : handler(tokens, database),
          dispatcher(server, [this](ConnectionId, const Request& request, std::optional<Session> session) {
              return handler.handle(request, session);
          }, 2, 32, 1024, [this](std::uint64_t user) { return handler.pending_messages(user); }),
          http(auth, database, "127.0.0.1", HttpConfig{0, 2}, std::move(files), [this](UserNotification notification) {
              server.post([this, notification = std::move(notification)] { dispatcher.notify(notification); });
          }) {
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

TEST_CASE("File notifications route online and replay offline until acknowledged", "[database][tcp][file]") {
    DatabaseFixture fixture;
    TempDirectory directory;
    RunningChat running(test_config(), FileConfig{directory.path.string(), 128 * 1024});
    const auto owner = fixture.store.create_user("owner", "test-hash");
    const auto online = fixture.store.create_user("online", "test-hash");
    const auto offline = fixture.store.create_user("offline", "test-hash");
    std::string message;
    {
        Peer peer(running.server.port());
        peer.send("authenticate", {{"token", running.tokens.issue(online)}});
        REQUIRE(peer.receive()["ok"] == true);
        httplib::Client http("127.0.0.1", running.http.port());
        const httplib::Headers headers{{"Authorization", "Bearer " + running.tokens.issue(owner)},
            {"X-File-Metadata", json{{"name", "notes.bin"}, {"recipients", {std::to_string(online), std::to_string(offline)}}}.dump()}};
        const auto uploaded = http.Post("/api/files", headers, std::string(100000, '\0'), "application/octet-stream");
        REQUIRE(uploaded);
        REQUIRE(uploaded->status == 201);
        const auto result = json::parse(uploaded->body);
        message = result["message_id"].get<std::string>();
        const auto notice = peer.receive();
        CHECK(notice["action"] == "file_available");
        CHECK(notice["data"]["message_id"] == message);
        CHECK(notice["data"]["file_id"] == result["file_id"]);
        CHECK(notice["data"]["size_bytes"] == 100000);
        // Disconnect without ACK; the same notice must replay on the next session.
    }
    fixture.store.save_direct_message(owner, "alongside-file", "text alongside the file", {offline});
    for (const auto user : {online, offline}) {
        Peer peer(running.server.port());
        peer.send("authenticate", {{"token", running.tokens.issue(user)}});
        REQUIRE(peer.receive()["ok"] == true);
        const auto notice = peer.receive();
        CHECK(notice["action"] == "file_available");
        CHECK(notice["data"]["message_id"] == message);
        peer.send("ack_message", {{"message_id", message}});
        if (user == offline) {
            const auto text = peer.receive();
            CHECK(text["action"] == "message");
            CHECK(text["data"]["text"] == "text alongside the file");
            peer.send("ack_message", {{"message_id", text["data"]["message_id"]}});
            CHECK(peer.receive()["ok"] == true);
        }
        CHECK(peer.receive()["ok"] == true);
        peer.send("ack_message", {{"message_id", message}});
        CHECK(peer.receive()["ok"] == true);
        CHECK(fixture.store.pending_messages(user).empty());
    }
    running.check();
}

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

json group_message_data(const std::string& client_id, const json& group, const std::string& text) {
    return {{"client_message_id", client_id}, {"group_id", group}, {"text", text}};
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

TEST_CASE("Members discover groups created while they were offline", "[database][tcp][group]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "unused-test-hash");
    const auto member = fixture.store.create_user("member", "unused-test-hash");
    const auto outsider = fixture.store.create_user("outsider", "unused-test-hash");
    fixture.store.create_group(owner, "Owner only", {});
    RunningChat running;
    Peer creator(running.server.port());
    authenticate(creator, running, owner);
    creator.send("create_group", {{"name", "Offline study"}, {"members", {std::to_string(member)}}});
    const auto created = creator.receive();
    REQUIRE(created["ok"] == true);
    const auto group = created["data"]["group_id"];
    const json expected = json::array({{{"group_id", group}, {"owner_id", std::to_string(owner)}, {"name", "Offline study"}}});
    {
        Peer peer(running.server.port());
        peer.send("list_groups");
        CHECK(peer.receive()["error"]["code"] == "authentication_required");
        authenticate(peer, running, member);
        peer.send("list_groups", {{"user_id", std::to_string(owner)}});
        const auto listed = peer.receive();
        REQUIRE(listed["ok"] == true);
        CHECK(listed["data"]["groups"] == expected);
        CHECK(listed["data"]["next_after_group_id"].is_null());
        peer.send("get_group_members", {{"group_id", listed["data"]["groups"][0]["group_id"]}});
        CHECK(peer.receive()["data"]["members"] == json::array({std::to_string(owner), std::to_string(member)}));
    }
    Peer reconnected(running.server.port()), other(running.server.port());
    authenticate(reconnected, running, member);
    reconnected.send("list_groups");
    CHECK(reconnected.receive()["data"]["groups"] == expected);
    authenticate(other, running, outsider);
    other.send("list_groups", {{"user_id", std::to_string(member)}});
    CHECK(other.receive()["data"] == json{{"groups", json::array()}, {"next_after_group_id", nullptr}});
    creator.send("list_groups");
    CHECK(creator.receive()["data"]["groups"].size() == 2);
    running.check();
}

TEST_CASE("Group listings paginate safely with fully escaped names", "[database][tcp][group]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "unused-test-hash");
    const auto member = fixture.store.create_user("member", "unused-test-hash");
    std::vector<std::string> ids;
    for (int i = 0; i < 65; ++i) {
        ids.push_back(std::to_string(fixture.store.create_group(owner, std::string(128, '\1'), {member})));
        if (i == 10) fixture.store.create_group(owner, "Not a member", {});
    }
    RunningChat running;
    Peer peer(running.server.port());
    authenticate(peer, running, member);
    peer.send("list_groups");
    const auto first = peer.receive();
    REQUIRE(first["ok"] == true);
    REQUIRE(first["data"]["groups"].size() == 64);
    CHECK(first["data"]["next_after_group_id"] == ids[63]);
    CHECK(first.dump().size() <= kMaxFrameSize);
    for (std::size_t i = 0; i < 64; ++i) {
        CHECK(first["data"]["groups"][i]["group_id"] == ids[i]);
        CHECK(first["data"]["groups"][i]["name"] == std::string(128, '\1'));
    }
    peer.send("list_groups", {{"after_group_id", first["data"]["next_after_group_id"]}});
    const auto second = peer.receive();
    REQUIRE(second["data"]["groups"].size() == 1);
    CHECK(second["data"]["groups"][0]["group_id"] == ids[64]);
    CHECK(second["data"]["next_after_group_id"].is_null());
    peer.send("list_groups", {{"after_group_id", ids.front()}});
    const auto exactly_full = peer.receive();
    CHECK(exactly_full["data"]["groups"].size() == 64);
    CHECK(exactly_full["data"]["next_after_group_id"].is_null());
    peer.send("list_groups", {{"after_group_id", ids.back()}});
    CHECK(peer.receive()["data"] == json{{"groups", json::array()}, {"next_after_group_id", nullptr}});
    peer.send("list_groups", {{"after_group_id", "18446744073709551615"}});
    CHECK(peer.receive()["data"]["groups"].empty());
    running.check();
}

TEST_CASE("Groups notify online members and share persisted messages with online and offline members",
          "[database][tcp][group][message]") {
    DatabaseFixture fixture;
    const auto alice = fixture.store.create_user("alice", "unused-test-hash");
    const auto bob = fixture.store.create_user("bob", "unused-test-hash");
    const auto carol = fixture.store.create_user("carol", "unused-test-hash");
    const auto outsider = fixture.store.create_user("outsider", "unused-test-hash");
    RunningChat running;
    Peer first(running.server.port()), second(running.server.port()), other(running.server.port());
    authenticate(first, running, alice);
    authenticate(second, running, bob);
    authenticate(other, running, outsider);
    first.send("create_group", {{"name", "Study group"}, {"owner_id", std::to_string(outsider)},
        {"members", {std::to_string(carol), std::to_string(bob), std::to_string(alice), std::to_string(bob)}}});
    const auto created = first.receive();
    REQUIRE(created["ok"] == true);
    const auto group = created["data"]["group_id"];
    const auto notice = second.receive();
    CHECK(notice["action"] == "group_created");
    CHECK(notice["data"]["group_id"] == group);
    CHECK(notice["data"]["owner_id"] == std::to_string(alice));
    CHECK(notice["data"]["name"] == "Study group");
    CHECK(notice["data"]["members"] == json::array({std::to_string(alice), std::to_string(bob), std::to_string(carol)}));
    CHECK(fixture.count("chat_groups") == 1);
    CHECK(fixture.count("group_members") == 3);
    second.send("get_group_members", {{"group_id", group}});
    const auto members = second.receive();
    CHECK(members["data"]["members"] == notice["data"]["members"]);
    other.send("get_group_members", {{"group_id", group}});
    CHECK(other.receive()["error"]["code"] == "not_group_member");
    other.send("send_group_message", group_message_data("bad", group, "Not a member"));
    CHECK(other.receive()["error"]["code"] == "not_group_member");
    first.send("get_group_members", {{"group_id", "18446744073709551615"}});
    CHECK(first.receive()["error"]["code"] == "not_group_member");
    CHECK(fixture.count("messages") == 0);

    auto data = group_message_data("g1", group, u8"Hello group \U0001f642");
    data["sender_id"] = std::to_string(outsider);
    data["recipients"] = {std::to_string(outsider)};
    first.send("send_group_message", data);
    const auto sent = first.receive();
    REQUIRE(sent["ok"] == true);
    CHECK(sent["data"]["group_id"] == group);
    const auto message = second.receive();
    CHECK(message["data"] == json{{"group_id", group}, {"message_id", sent["data"]["message_id"]},
        {"sender_id", std::to_string(alice)}, {"client_message_id", "g1"}, {"text", data["text"]}});
    CHECK(fixture.count("message_deliveries") == 2);
    CHECK(fixture.store.pending_messages(alice).empty());
    REQUIRE(fixture.store.pending_messages(carol).size() == 1);
    CHECK(std::to_string(*fixture.store.pending_messages(carol)[0].group_id) == group);
    other.send("ping");
    CHECK(other.receive()["data"]["pong"] == true);
    first.send("ping");
    CHECK(first.receive()["data"]["pong"] == true);
    second.send("ack_message", {{"message_id", sent["data"]["message_id"]}});
    CHECK(second.receive()["ok"] == true);
    Peer reconnected(running.server.port());
    authenticate(reconnected, running, carol, false);
    CHECK(reconnected.receive() == message);
    reconnected.send("ack_message", {{"message_id", sent["data"]["message_id"]}});
    CHECK(reconnected.receive()["ok"] == true);
    CHECK(fixture.store.pending_messages(carol).empty());

    second.send("send_group_message", group_message_data("reply", group, "Hi Alice"));
    CHECK(second.receive()["ok"] == true);
    CHECK(first.receive()["data"]["sender_id"] == std::to_string(bob));
    CHECK(reconnected.receive()["data"]["text"] == "Hi Alice");
    running.check();
}

TEST_CASE("Group retries broadcast only the original delivery snapshot and reject changed messages",
          "[database][tcp][group][message]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "unused-test-hash");
    const auto member = fixture.store.create_user("member", "unused-test-hash");
    const auto newcomer = fixture.store.create_user("newcomer", "unused-test-hash");
    const auto group_id = fixture.store.create_group(owner, "Study", {member});
    const auto group = std::to_string(group_id);
    const auto another = std::to_string(fixture.store.create_group(owner, "Another", {member}));
    RunningChat running;
    Peer first(running.server.port()), second(running.server.port()), third(running.server.port());
    authenticate(first, running, owner);
    authenticate(second, running, member);
    authenticate(third, running, newcomer);
    first.send("send_group_message", group_message_data("same-id", group, "Original"));
    const auto sent = first.receive();
    REQUIRE(sent["ok"] == true);
    const auto notice = second.receive();
    second.send("ack_message", {{"message_id", sent["data"]["message_id"]}});
    CHECK(second.receive()["ok"] == true);
    fixture.database.execute("INSERT INTO group_members (group_id, user_id) VALUES (?, ?)", {group_id, newcomer});
    fixture.database.execute("DELETE FROM group_members WHERE group_id = ? AND user_id = ?", {group_id, member});
    first.send("send_group_message", group_message_data("same-id", group, "Original"));
    CHECK(first.receive()["data"] == sent["data"]);
    CHECK(second.receive() == notice);
    third.send("ping");
    CHECK(third.receive()["data"]["pong"] == true);
    CHECK(fixture.store.pending_messages(newcomer).empty());
    CHECK(fixture.store.pending_messages(member).empty());
    CHECK(fixture.count("messages") == 1);
    CHECK(fixture.count("message_deliveries") == 1);
    for (const auto& changed : {group_message_data("same-id", group, "Changed"),
                                 group_message_data("same-id", another, "Original")}) {
        first.send("send_group_message", changed);
        CHECK(first.receive()["error"]["code"] == "message_conflict");
    }
    first.send("send_message", message_data("same-id", {member}, "Original"));
    CHECK(first.receive()["error"]["code"] == "message_conflict");
    second.send("send_group_message", group_message_data("removed", group, "No longer a member"));
    CHECK(second.receive()["error"]["code"] == "not_group_member");
    CHECK(fixture.count("messages") == 1);
    running.check();
}

TEST_CASE("Group creation rejects missing users and supports an owner-only group", "[database][tcp][group]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "unused-test-hash");
    RunningChat running;
    Peer peer(running.server.port());
    authenticate(peer, running, owner);
    peer.send("create_group", {{"name", "Missing member"}, {"members", {"18446744073709551615"}}});
    CHECK(peer.receive()["error"]["code"] == "invalid_recipient");
    CHECK(fixture.count("chat_groups") == 0);
    CHECK(fixture.count("group_members") == 0);
    peer.send("create_group", {{"name", "Solo"}});
    const auto created = peer.receive();
    REQUIRE(created["ok"] == true);
    const auto group = created["data"]["group_id"];
    peer.send("get_group_members", {{"group_id", group}});
    CHECK(peer.receive()["data"]["members"] == json::array({std::to_string(owner)}));
    peer.send("send_group_message", group_message_data(std::string(64, '\1'), group, std::string(4096, '\0')));
    CHECK(peer.receive()["ok"] == true);
    peer.send("ping");
    CHECK(peer.receive()["data"]["pong"] == true);
    CHECK(fixture.count("messages") == 1);
    CHECK(fixture.count("message_deliveries") == 0);
    running.check();
}

TEST_CASE("Group creation accepts 64 total members and a 128-byte name", "[database][tcp][group]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "unused-test-hash");
    auto members = json::array({std::to_string(owner)});
    for (int i = 0; i < 63; ++i) {
        members.push_back(std::to_string(fixture.store.create_user("member-" + std::to_string(i), "unused-test-hash")));
    }
    RunningChat running;
    Peer peer(running.server.port());
    authenticate(peer, running, owner);
    peer.send("create_group", {{"name", std::string(128, 'x')}, {"members", members}});
    const auto created = peer.receive();
    REQUIRE(created["ok"] == true);
    peer.send("get_group_members", {{"group_id", created["data"]["group_id"]}});
    CHECK(peer.receive()["data"]["members"] == members);
    CHECK(fixture.count("group_members") == 64);
    running.check();
}
