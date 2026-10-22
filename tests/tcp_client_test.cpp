#include "net/tcp_client.h"
#include "auth_client.h"
#include "protocol.h"
#include "file_client.h"
#include "pending_file.h"
#include "temp_directory.h"

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
#include <fstream>
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
    explicit ConsoleProcess(std::uint16_t port, std::uint16_t http_port = 8080, bool json_output = true) {
        auto input = pipe();
        auto output = pipe();
        const auto port_text = std::to_string(port);
        const auto http_text = std::to_string(http_port);
        pid_ = ::fork();
        check_socket(pid_, "fork");
        if (pid_ == 0) {
            if (::dup2(input[0].get(), STDIN_FILENO) < 0 ||
                ::dup2(output[1].get(), STDOUT_FILENO) < 0) ::_exit(127);
            if (json_output) {
                ::execl(CHATROOM_CLIENT_PATH, CHATROOM_CLIENT_PATH, "--json", "127.0.0.1",
                        port_text.c_str(), http_text.c_str(), nullptr);
            } else {
                ::execl(CHATROOM_CLIENT_PATH, CHATROOM_CLIENT_PATH, "127.0.0.1",
                        port_text.c_str(), http_text.c_str(), nullptr);
            }
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

class TestAuthServer {
public:
    explicit TestAuthServer(httplib::Server::Handler handler) {
        server_.new_task_queue = [] { return new httplib::ThreadPool(1); };
        server_.Post("/api/users", handler);
        server_.Post("/api/auth/token", handler);
        server_.Post("/api/files", handler);
        server_.Get("/api/files/7", std::move(handler));
        const int port = server_.bind_to_any_port("127.0.0.1");
        if (port < 0) throw std::runtime_error("Cannot bind the test authentication server");
        port_ = static_cast<std::uint16_t>(port);
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~TestAuthServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }
    std::uint16_t port() const { return port_; }

private:
    httplib::Server server_;
    std::uint16_t port_;
    std::thread thread_;
};
} // namespace

TEST_CASE("Pending files clean up failures and never overwrite existing paths", "[client][file]") {
    TempDirectory directory;
    const auto path = directory.path / "file.bin";
    {
        PendingFile file(path);
        file.append("partial");
        CHECK(std::filesystem::exists(path));
    }
    CHECK_FALSE(std::filesystem::exists(path));
    {
        PendingFile file(path);
        file.append("complete");
        file.finish();
        file.keep();
    }
    CHECK_THROWS_AS(PendingFile(path), std::system_error);
    CHECK(std::filesystem::file_size(path) == 8);
}

TEST_CASE("The file client streams uploads and downloads without overwriting local files", "[client][http][file]") {
    TempDirectory directory;
    const auto input = directory.path / "notes final.bin";
    const auto output = directory.path / "download.bin";
    std::string bytes;
    SECTION("Binary file") { bytes = std::string(100000, 'x') + std::string("\0\xff\n", 3); }
    SECTION("Empty file") {}
    { std::ofstream file(input, std::ios::binary); file.write(bytes.data(), bytes.size()); }
    TestAuthServer server([&](const httplib::Request& request, httplib::Response& response) {
        if (request.get_header_value("Authorization") != "Bearer file-test-token") { response.status = 401; return; }
        if (request.method == "GET") { response.set_content(bytes, "application/octet-stream"); return; }
        const auto metadata = parse_json(request.get_header_value("X-File-Metadata"));
        if (metadata != nlohmann::json{{"name", "notes final.bin"}, {"recipients", {"2", "3"}}} || request.body != bytes) {
            response.status = 400;
            return;
        }
        response.status = 201;
        response.set_content(R"({"file_id":"7","message_id":"9"})", "application/json");
    });
    FileClient files("127.0.0.1", server.port());
    CHECK(files.upload("file-test-token", input, {"2", "3"})["file_id"] == "7");
    files.download("file-test-token", "7", output);
    std::ifstream downloaded(output, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(downloaded), {}) == bytes);
    CHECK_THROWS_AS(files.download("file-test-token", "7", output), std::system_error);
    CHECK(std::filesystem::file_size(output) == bytes.size());
    CHECK_THROWS_AS(files.upload("", input, {"2"}), std::runtime_error);
    CHECK_THROWS_AS(files.upload("file-test-token", directory.path, {"2"}), std::runtime_error);
}

TEST_CASE("File download failures remove partial output and report JSON errors", "[client][http][file]") {
    TempDirectory directory;
    bool truncated = false;
    SECTION("HTTP error") {}
    SECTION("Connection closes during the body") { truncated = true; }
    TestAuthServer server([&](const httplib::Request&, httplib::Response& response) {
        if (truncated) {
            response.set_content_provider(100, "application/octet-stream",
                [](std::size_t, std::size_t, httplib::DataSink& sink) { sink.write("short", 5); return false; });
        } else {
            response.status = 404;
            response.set_content(R"({"error":{"code":"file_not_found","message":"File is unavailable"}})", "application/json");
        }
    });
    FileClient files("127.0.0.1", server.port());
    const auto output = directory.path / "missing.bin";
    if (truncated) CHECK_THROWS_AS(files.download("file-test-token", "7", output), std::runtime_error);
    else CHECK_THROWS_WITH(files.download("file-test-token", "7", output), "file_not_found: File is unavailable");
    CHECK_FALSE(std::filesystem::exists(output));
}

TEST_CASE("Console file commands retain the successful login and acknowledge duplicate file notices", "[client][console][file]") {
    TempDirectory directory;
    const auto input = directory.path / "notes final.bin";
    const auto output = directory.path / "download final.bin";
    { std::ofstream file(input, std::ios::binary); file << "file bytes"; }
    int logins = 0;
    TestAuthServer http([&](const httplib::Request& request, httplib::Response& response) {
        if (request.path == "/api/auth/token") {
            response.set_content(nlohmann::json{{"access_token", ++logins == 1 ? "first-token" : "second-token"}}.dump(), "application/json");
        } else if (request.get_header_value("Authorization") != "Bearer first-token") {
            response.status = 401;
        } else if (request.method == "GET") response.set_content("file bytes", "application/octet-stream");
        else {
            response.status = 201;
            response.set_content(R"({"file_id":"7","message_id":"9"})", "application/json");
        }
    });
    TestPeer peer([](int socket) {
        const auto login = parse_request(receive_payload(socket));
        send_bytes(socket, encode_frame(make_response(login, {{"user_id", "1"}}).dump()));
        const auto failed = parse_request(receive_payload(socket));
        send_bytes(socket, encode_frame(make_error_response(failed, "already_authenticated", "Reconnect to change users").dump()));
        const auto notice = make_notification("file_available", {{"message_id", "9"}, {"file_id", "7"},
            {"sender_id", "2"}, {"name", "notes final.bin"}, {"content_type", "application/octet-stream"}, {"size_bytes", 10}}).dump();
        send_bytes(socket, encode_frame(notice) + encode_frame(notice));
        int acknowledgments = 0;
        bool pinged = false;
        // Terminal requests may be interleaved with automatic ACKs.
        for (int i = 0; i < 3; ++i) {
            const auto request = parse_request(receive_payload(socket));
            if (request.action == "ack_message" && request.data["message_id"] == "9") {
                ++acknowledgments;
                send_bytes(socket, encode_frame(make_response(request, {{"message_id", "9"}}).dump()));
            } else if (request.action == "ping") {
                pinged = true;
                send_bytes(socket, encode_frame(make_response(request, {{"pong", true}}).dump()));
            } else throw std::runtime_error("Unexpected request during file commands");
        }
        if (acknowledgments != 2 || !pinged) throw std::runtime_error("Missing file ACK or ping");
    });
    ConsoleProcess console(peer.port(), http.port(), false);
    console.write("/login alice example-password\n");
    console.read_until("Authenticated as user 1.");
    console.write("/login bob example-password\n");
    console.read_until("Reconnect to change users");
    console.write("/upload 2 \"" + input.string() + "\"\n");
    console.read_until("Uploaded file 7.");
    console.write("/download 7 \"" + output.string() + "\"\n");
    console.read_until("Downloaded file 7 to ");
    CHECK(std::filesystem::file_size(output) == 10);
    console.write("/ping\n");
    peer.check();
    const auto text = console.read_until("Pong.");
    const auto marker = "[file 7, message 9]";
    CHECK(text.find(marker) != std::string::npos);
    CHECK(text.find(marker, text.find(marker) + 1) == std::string::npos);
    CHECK(text.find("first-token") == std::string::npos);
    CHECK(text.find("second-token") == std::string::npos);
    console.check();
}

TEST_CASE("HTTP client sends exact credentials and validates registration and login results", "[client][http]") {
    TestAuthServer server([](const httplib::Request& request, httplib::Response& response) {
        const auto data = parse_json(request.body);
        if (request.get_header_value("Content-Type") != "application/json" ||
            data != nlohmann::json{{"username", "name with spaces"}, {"password", "a password"}}) {
            response.status = 400;
            response.set_content(R"({"error":{"code":"bad_test_request","message":"Credentials changed"}})", "application/json");
        } else if (request.path == "/api/users") {
            response.status = 201;
            response.set_content(R"({"id":"7","username":"name with spaces"})", "application/json");
        } else {
            response.set_content(R"({"access_token":"test-token","token_type":"Bearer","expires_in":60})", "application/json");
        }
    });
    AuthClient auth("127.0.0.1", server.port());
    CHECK(auth.register_user("name with spaces", "a password") == "7");
    CHECK(auth.login("name with spaces", "a password") == "test-token");
    CHECK_THROWS_AS(auth.login("", "a password"), std::invalid_argument);
    CHECK_THROWS_AS(auth.register_user("alice", "short"), std::invalid_argument);
}

TEST_CASE("HTTP client reports service errors and rejects malformed authentication responses", "[client][http]") {
    int status = 401;
    std::string body = R"({"error":{"code":"invalid_credentials","message":"Invalid username or password"}})";
    std::string expected = "invalid_credentials: Invalid username or password";
    SECTION("Service error") {}
    SECTION("Unexpected HTTP status") { status = 503; body = "{}"; expected = "Authentication HTTP status 503"; }
    SECTION("Malformed JSON") { body = "{broken"; expected = "Authentication service returned invalid JSON"; }
    SECTION("Excessive nesting") { body = std::string(65, '[') + "0" + std::string(65, ']'); expected = "Authentication service returned invalid JSON"; }
    SECTION("Wrong root type") { body = "[]"; expected = "Authentication service returned an invalid response"; }
    SECTION("Missing token") { status = 200; body = "{}"; expected = "Login response is missing a valid access token"; }
    SECTION("Wrong token type") { status = 200; body = R"({"access_token":7})"; expected = "Login response is missing a valid access token"; }
    SECTION("Oversized token") { status = 200; body = nlohmann::json{{"access_token", std::string(4097, 'x')}}.dump(); expected = "Login response is missing a valid access token"; }
    TestAuthServer server([status, body](const auto&, auto& response) {
        response.status = status;
        response.set_content(body, "application/json");
    });
    AuthClient auth("127.0.0.1", server.port());
    CHECK_THROWS_WITH(auth.login("alice", "a password"), expected);
}

TEST_CASE("HTTP registration reports conflicts and rejects invalid user ID responses", "[client][http]") {
    int status = 409;
    std::string body = R"({"error":{"code":"username_taken","message":"Username is already registered"}})";
    std::string expected = "username_taken: Username is already registered";
    SECTION("Duplicate username") {}
    SECTION("Missing ID") { status = 201; body = "{}"; expected = "Registration response is missing the user ID"; }
    SECTION("Numeric ID") { status = 201; body = R"({"id":7})"; expected = "Registration response is missing the user ID"; }
    SECTION("Empty ID") { status = 201; body = R"({"id":""})"; expected = "Registration response is missing the user ID"; }
    TestAuthServer server([status, body](const auto&, auto& response) {
        response.status = status;
        response.set_content(body, "application/json");
    });
    AuthClient auth("127.0.0.1", server.port());
    CHECK_THROWS_WITH(auth.register_user("alice", "a password"), expected);
}

TEST_CASE("HTTP authentication bounds response size and reports unavailable endpoints", "[client][http]") {
    SECTION("Oversized response") {
        TestAuthServer server([](const auto&, auto& response) {
            response.set_content(std::string(9000, 'x'), "application/json");
        });
        AuthClient auth("127.0.0.1", server.port());
        CHECK_THROWS_AS(auth.login("alice", "a password"), std::runtime_error);
    }
    SECTION("Connection refused") {
        UniqueFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        REQUIRE(socket.get() >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t size = sizeof(address);
        REQUIRE(::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &size) == 0);
        AuthClient auth("127.0.0.1", ntohs(address.sin_port));
        CHECK_THROWS_AS(auth.login("alice", "a password"), std::runtime_error);
    }
}

TEST_CASE("Console commands register, log in, authenticate TCP, and display chat without tokens",
          "[client][console][commands]") {
    TestAuthServer http([](const auto& request, auto& response) {
        const auto credentials = parse_json(request.body);
        if (credentials["username"] != "alice with spaces" || credentials["password"] != "a password") {
            response.status = 401;
            response.set_content(R"({"error":{"code":"invalid_credentials","message":"Invalid username or password"}})", "application/json");
        } else if (request.path == "/api/users") {
            response.status = 201;
            response.set_content(R"({"id":"7"})", "application/json");
        } else {
            response.set_content(R"({"access_token":"private-test-token"})", "application/json");
        }
    });
    TestPeer peer([](int fd) {
        const auto auth = parse_request(receive_payload(fd));
        if (auth.action != "authenticate" || auth.data["token"] != "private-test-token") {
            throw std::runtime_error("Login did not authenticate TCP");
        }
        send_bytes(fd, encode_frame(make_response(auth, {{"user_id", "7"}}).dump()));
        const auto message = parse_request(receive_payload(fd));
        if (message.action != "send_message" || message.data["recipients"] != nlohmann::json::array({"8", "9"}) ||
            message.data["text"] != "Hello both users" || message.data["client_message_id"].get<std::string>().size() > 64) {
            throw std::runtime_error("Incorrect outgoing message");
        }
        send_bytes(fd, encode_frame(make_response(message, {{"message_id", "101"}}).dump()));
        send_bytes(fd, encode_frame(make_notification("message", {{"message_id", "102"}, {"sender_id", "8"},
            {"client_message_id", "reply"}, {"text", "Hi\nthere\x1b"}}).dump()));
        for (int i = 0; i < 2; ++i) {
            const auto request = parse_request(receive_payload(fd));
            if (request.action == "ack_message") send_bytes(fd, encode_frame(make_response(request).dump()));
            else if (request.action == "ping") send_bytes(fd, encode_frame(make_response(request, {{"pong", true}}).dump()));
            else throw std::runtime_error("Unexpected command request");
        }
    });
    ConsoleProcess console(peer.port(), http.port(), false);
    console.write("/register \"alice with spaces\" \"a password\"\n");
    console.read_until("Registered user 7");
    console.write("/login \"alice with spaces\" wrong-password\n");
    console.read_until("invalid_credentials");
    console.write("/login \"alice with spaces\" \"a password\"\n");
    console.read_until("Authenticated as user 7");
    console.write("/send 8,9 Hello both users\n");
    console.read_until("user 8: Hi\\nthere\\u001b");
    console.write("/ping\n");
    const auto& output = console.read_until("Disconnected.");
    CHECK(output.find("Sent message 101") != std::string::npos);
    CHECK(output.find("Pong.") != std::string::npos);
    CHECK(output.find("private-test-token") == std::string::npos);
    CHECK(output.find("\x1b") == std::string::npos);
    console.check();
    peer.check();
}

TEST_CASE("Invalid console commands leave the connection usable", "[client][console][commands]") {
    TestPeer peer([](int fd) {
        const auto request = parse_request(receive_payload(fd));
        if (request.action != "ping") throw std::runtime_error("Invalid command reached the server");
        send_bytes(fd, encode_frame(make_response(request, {{"pong", true}}).dump()));
    });
    ConsoleProcess console(peer.port(), 8080, false);
    console.write("/help\n/unknown\n/login alice\n/register \"unfinished\n/send 0 Hello\n/send 8, Hello\n/ping extra\n");
    console.write("/create_group\n/create_group \"\"\n/create_group Study 0\n/members 0\n/members 1 extra\n/send_group 01 Hello\n/send_group 1\n");
    console.write("/send 8 " + std::string(4097, 'x') + "\n/ping\n");
    const auto& output = console.read_until("Disconnected.");
    CHECK(output.find("Commands:") != std::string::npos);
    CHECK(output.find("Unknown command") != std::string::npos);
    CHECK(output.find("Usage: /login") != std::string::npos);
    CHECK(output.find("Pong.") != std::string::npos);
    console.check();
    peer.check();
}

TEST_CASE("Console group commands and notifications reuse message ACKs", "[client][console][group]") {
    TestPeer peer([](int fd) {
        const auto create = parse_request(receive_payload(fd));
        if (create.action != "create_group" || create.data["name"] != "Study group" ||
            create.data["members"] != nlohmann::json::array({"8", "9"})) {
            throw std::runtime_error("Incorrect group creation command");
        }
        send_bytes(fd, encode_frame(make_response(create, {{"group_id", "42"}}).dump()));
        send_bytes(fd, encode_frame(make_notification("group_created", {{"group_id", "50"},
            {"owner_id", "8"}, {"name", "Friends"}, {"members", {"7", "8"}}}).dump()));
        const auto members = parse_request(receive_payload(fd));
        if (members.action != "get_group_members" || members.data["group_id"] != "42") {
            throw std::runtime_error("Incorrect group members command");
        }
        send_bytes(fd, encode_frame(make_response(members, {{"group_id", "42"}, {"members", {"7", "8", "9"}}}).dump()));
        const auto message = parse_request(receive_payload(fd));
        if (message.action != "send_group_message" || message.data["group_id"] != "42" ||
            message.data["text"] != "Hello group" || message.data["client_message_id"].get<std::string>().empty() ||
            message.data["client_message_id"].get<std::string>().size() > 64) {
            throw std::runtime_error("Incorrect group send command");
        }
        send_bytes(fd, encode_frame(make_response(message, {{"message_id", "105"}, {"group_id", "42"}}).dump()));
        const auto notice = make_notification("message", {{"group_id", "42"}, {"message_id", "106"},
            {"sender_id", "8"}, {"client_message_id", "reply"}, {"text", "Group reply"}}).dump();
        send_bytes(fd, encode_frame(notice) + encode_frame(notice));
        for (int i = 0; i < 2; ++i) {
            const auto ack = parse_request(receive_payload(fd));
            if (ack.action != "ack_message" || ack.data["message_id"] != "106") {
                throw std::runtime_error("Group messages were not acknowledged");
            }
            send_bytes(fd, encode_frame(make_response(ack).dump()));
        }
    });
    ConsoleProcess console(peer.port(), 8080, false);
    console.write("/create_group \"Study group\" 8,9\n");
    console.read_until("Created group 42.");
    console.write("/members 42\n");
    console.read_until("Group 42 members: 7, 8, 9.");
    console.write("/send_group 42 Hello group\n");
    const auto& output = console.read_until("Disconnected.");
    CHECK(output.find("Added to group 50: Friends (owner 8).") != std::string::npos);
    CHECK(output.find("Sent message 105 to group 42.") != std::string::npos);
    const auto first = output.find("[group 42, message 106] user 8: Group reply");
    REQUIRE(first != std::string::npos);
    CHECK(output.find("Group reply", first + std::string("[group 42, message 106] user 8: Group reply").size()) == std::string::npos);
    console.check();
    peer.check();
}

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
