#include "database_fixture.h"
#include "auth/password.h"
#include "http/http_server.h"

#include <future>
#include <iostream>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <sstream>

using namespace chatroom;
using nlohmann::json;

namespace {
const std::string kTestSecret(32, 'k');

struct RunningAuth {
    TokenService tokens{kTestSecret, 60};
    AuthService auth{tokens};
    HttpServer http;

    explicit RunningAuth(DatabaseConfig database = test_config())
        : http(auth, std::move(database), "127.0.0.1", HttpConfig{0, 2}) { http.start(); }

    void finish() {
        http.stop();
        http.check();
    }
};

// Restore stderr only after HTTP workers have joined.
struct LogCapture {
    std::ostringstream output;
    std::streambuf* previous = std::clog.rdbuf(output.rdbuf());
    ~LogCapture() { std::clog.rdbuf(previous); }
};
} // namespace

TEST_CASE("HTTP registration and login persist hashes and issue valid JWTs", "[database][http][auth]") {
    DatabaseFixture fixture;
    RunningAuth running;
    httplib::Client client("127.0.0.1", running.http.port());
    const json credentials{{"username", "alice"}, {"password", "correct-password"}};
    const auto registered = client.Post("/api/users", credentials.dump(), "application/json");
    REQUIRE(registered);
    REQUIRE(registered->status == 201);
    const auto user = json::parse(registered->body);
    CHECK(user["username"] == "alice");
    CHECK(user["id"].is_string());
    CHECK(user.size() == 2);
    const auto stored = fixture.store.user_by_name("alice");
    REQUIRE(stored);
    CHECK(user["id"] == std::to_string(stored->id));
    CHECK(stored->password_hash != "correct-password");
    CHECK(verify_password("correct-password", stored->password_hash));

    const auto logged_in = client.Post("/api/auth/token", credentials.dump(), "application/json");
    REQUIRE(logged_in);
    REQUIRE(logged_in->status == 200);
    CHECK(logged_in->get_header_value("Cache-Control") == "no-store");
    const auto body = json::parse(logged_in->body);
    CHECK(body["token_type"] == "Bearer");
    CHECK(body["expires_in"] == 60);
    CHECK(body.size() == 3);
    const auto token = jwt::decode(body["access_token"].get<std::string>());
    CHECK_NOTHROW(jwt::verify().allow_algorithm(jwt::algorithm::hs256{kTestSecret})
                      .with_issuer("chatroom-v2").with_type("JWT").verify(token));
    CHECK(token.get_subject() == std::to_string(stored->id));
    CHECK(token.get_expires_at() - token.get_issued_at() == std::chrono::seconds(60));
    running.finish();
}

TEST_CASE("HTTP authentication handles duplicate names and incorrect credentials", "[database][http]") {
    DatabaseFixture fixture;
    RunningAuth running;
    httplib::Client client("127.0.0.1", running.http.port());
    const json credentials{{"username", "alice"}, {"password", "correct-password"}};
    REQUIRE(client.Post("/api/users", credentials.dump(), "application/json")->status == 201);
    const auto duplicate = client.Post("/api/users", credentials.dump(), "application/json");
    REQUIRE(duplicate);
    CHECK(duplicate->status == 409);
    CHECK(json::parse(duplicate->body)["error"]["code"] == "username_taken");
    std::string error_body;
    for (const auto* name : {"alice", "missing"}) {
        const auto response = client.Post("/api/auth/token",
            json{{"username", name}, {"password", "wrong-password"}}.dump(), "application/json");
        REQUIRE(response);
        CHECK(response->status == 401);
        CHECK(json::parse(response->body)["error"]["code"] == "invalid_credentials");
        if (error_body.empty()) error_body = response->body;
        else CHECK(response->body == error_body);
    }
    CHECK(fixture.count("users") == 1);
    running.finish();
}

TEST_CASE("HTTP input errors stay bounded and return JSON", "[database][http]") {
    DatabaseFixture fixture;
    RunningAuth running;
    httplib::Client client("127.0.0.1", running.http.port());
    const std::string deep = R"({"ignored":)" + std::string(64, '[') + "0" + std::string(64, ']') + "}";
    for (const auto& body : {std::string("{"), std::string("[]"), std::string("{}"),
                             std::string(1, '\xff'), std::string(R"({"username":1,"password":"valid-password"})"), deep}) {
        const auto response = client.Post("/api/users", body, "application/json");
        REQUIRE(response);
        CHECK(response->status == 400);
        CHECK(json::parse(response->body).contains("error"));
    }
    const auto media = client.Post("/api/users", "{}", "text/plain");
    REQUIRE(media);
    CHECK(media->status == 415);
    const auto large = client.Post("/api/users", std::string(4097, 'x'), "application/json");
    REQUIRE(large);
    CHECK(large->status == 413);
    CHECK(json::parse(large->body)["error"]["code"] == "payload_too_large");
    const auto method = client.Get("/api/auth/token");
    REQUIRE(method);
    CHECK(method->status == 405);
    CHECK(method->get_header_value("Allow") == "POST");
    const auto missing = client.Get("/missing");
    REQUIRE(missing);
    CHECK(missing->status == 404);
    CHECK(json::parse(missing->body)["error"]["code"] == "not_found");
    CHECK(fixture.count("users") == 0);
    running.finish();
}

TEST_CASE("HTTP credentials enforce documented byte boundaries", "[database][http]") {
    DatabaseFixture fixture;
    RunningAuth running;
    httplib::Client client("127.0.0.1", running.http.port());
    for (const auto& credentials : {json{{"username", ""}, {"password", "valid-password"}},
                                    json{{"username", std::string(65, 'u')}, {"password", "valid-password"}},
                                    json{{"username", "valid"}, {"password", "1234567"}},
                                    json{{"username", "valid"}, {"password", std::string(129, 'p')}}}) {
        const auto response = client.Post("/api/users", credentials.dump(), "application/json");
        REQUIRE(response);
        CHECK(response->status == 400);
    }
    for (const auto& credentials : {json{{"username", "u"}, {"password", "12345678"}},
                                    json{{"username", std::string(64, 'u')}, {"password", std::string(128, 'p')}}}) {
        const auto response = client.Post("/api/users", credentials.dump(), "Application/JSON; charset=utf-8");
        REQUIRE(response);
        CHECK(response->status == 201);
        const auto login = client.Post("/api/auth/token", credentials.dump(), "application/json");
        REQUIRE(login);
        CHECK(login->status == 200);
    }
    CHECK(fixture.count("users") == 2);
    running.finish();
}

TEST_CASE("Concurrent HTTP registrations create one user", "[database][http]") {
    DatabaseFixture fixture;
    RunningAuth running;
    const auto port = running.http.port();
    std::promise<void> start;
    const auto ready = start.get_future().share();
    std::vector<std::future<int>> results;
    for (int i = 0; i < 4; ++i) {
        results.push_back(std::async(std::launch::async, [=] {
            httplib::Client client("127.0.0.1", port);
            client.set_read_timeout(10);
            ready.wait();
            const auto response = client.Post("/api/users",
                R"({"username":"same-user","password":"valid-password"})", "application/json");
            return response ? response->status : 0;
        }));
    }
    start.set_value();
    int created = 0;
    for (auto& result : results) {
        const auto status = result.get();
        CHECK((status == 201 || status == 409));
        if (status == 201) ++created;
    }
    CHECK(created == 1);
    CHECK(fixture.count("users") == 1);
    running.finish();
}

TEST_CASE("HTTP logs contain request metadata without credentials or tokens", "[database][http]") {
    DatabaseFixture fixture;
    LogCapture capture;
    RunningAuth running;
    httplib::Client client("127.0.0.1", running.http.port());
    const std::string body = R"({"username":"log-user","password":"private-test-password"})";
    REQUIRE(client.Post("/api/users", body, "application/json")->status == 201);
    const auto login = client.Post("/api/auth/token?password=query-secret", body, "application/json");
    REQUIRE(login);
    REQUIRE(login->status == 200);
    const auto token = json::parse(login->body)["access_token"].get<std::string>();
    REQUIRE(client.Post("/api/auth/token", "{}", "application/json")->status == 400);
    running.finish();
    const auto logs = capture.output.str();
    CHECK(logs.find("private-test-password") == std::string::npos);
    CHECK(logs.find("query-secret") == std::string::npos);
    CHECK(logs.find(token) == std::string::npos);
    std::istringstream lines(logs);
    std::string line;
    int requests = 0;
    while (std::getline(lines, line)) {
        const auto record = json::parse(line);
        if (record["event"] != "http_request") continue;
        ++requests;
        CHECK(record["timestamp"].is_string());
        const auto& context = record["context"];
        CHECK(context["client_ip"] == "127.0.0.1");
        CHECK(context["method"] == "POST");
        CHECK(context["uri"].is_string());
        CHECK(context["status"].is_number_integer());
        if (context["status"] == 400) CHECK(context["error"]["code"] == "invalid_request");
    }
    CHECK(requests == 3);
}

TEST_CASE("HTTP database failures return generic errors and keep the listener alive", "[database][http]") {
    DatabaseFixture fixture;
    auto config = test_config();
    config.password = "deliberately-incorrect-http-test-password";
    RunningAuth running(config);
    httplib::Client client("127.0.0.1", running.http.port());
    const auto response = client.Post("/api/auth/token",
        R"({"username":"user","password":"valid-password"})", "application/json");
    REQUIRE(response);
    CHECK(response->status == 500);
    CHECK(json::parse(response->body)["error"]["code"] == "internal_error");
    CHECK(response->body.find(config.password) == std::string::npos);
    const auto next = client.Get("/missing");
    REQUIRE(next);
    CHECK(next->status == 404);
    running.finish();
}

TEST_CASE("HTTP shutdown joins idle workers and releases its listening port", "[database][http]") {
    RunningAuth running;
    const auto port = running.http.port();
    running.finish();
    HttpServer replacement(running.auth, test_config(), "127.0.0.1", HttpConfig{port, 1});
    replacement.start();
    replacement.stop();
    CHECK_NOTHROW(replacement.check());
}
