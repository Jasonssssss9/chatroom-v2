#include "database_fixture.h"
#include "http/http_server.h"
#include "temp_directory.h"

#include <fstream>

using namespace chatroom;
using nlohmann::json;

namespace {
struct RunningFiles {
    TempDirectory directory;
    TokenService tokens{std::string(32, 'k'), 60};
    AuthService auth{tokens};
    HttpServer http;

    explicit RunningFiles(std::uint64_t maximum = 128 * 1024)
        : http(auth, test_config(), "127.0.0.1", HttpConfig{0, 2}, FileConfig{directory.path.string(), maximum}) {
        http.start();
    }
    ~RunningFiles() { http.stop(); }
    httplib::Headers headers(std::uint64_t user, json metadata = {{"name", "notes.bin"}, {"recipients", json::array()}}) {
        return {{"Authorization", "Bearer " + tokens.issue(user)}, {"X-File-Metadata", metadata.dump(-1, ' ', true)}};
    }
    std::size_t file_count() const {
        return std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator{});
    }
};
} // namespace

TEST_CASE("HTTP files stream binary data and enforce owner and recipient access", "[database][http][file]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    const auto recipient = fixture.store.create_user("recipient", "test-hash");
    const auto outsider = fixture.store.create_user("outsider", "test-hash");
    RunningFiles running;
    httplib::Client client("127.0.0.1", running.http.port());
    std::string bytes(100000, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 256);
    const std::string name = u8"notes \u00e9 final.bin";
    const auto upload = client.Post("/api/files", running.headers(owner,
        {{"name", name}, {"recipients", {std::to_string(recipient), std::to_string(recipient)}}}), bytes, "application/octet-stream");
    REQUIRE(upload);
    REQUIRE(upload->status == 201);
    const auto data = json::parse(upload->body);
    const auto id = std::stoull(data.at("file_id").get<std::string>());
    CHECK(data["name"] == name);
    CHECK(data["size_bytes"] == bytes.size());
    CHECK_FALSE(data.contains("storage_key"));
    CHECK(upload->get_header_value("Location") == "/api/files/" + std::to_string(id));
    const auto stored = fixture.store.file_for_user(id, owner);
    REQUIRE(stored);
    CHECK(stored->storage_key.size() == 32);
    CHECK(stored->storage_key != name);
    CHECK(std::filesystem::file_size(running.directory.path / stored->storage_key) == bytes.size());
    CHECK(running.file_count() == 1);
    for (const auto user : {owner, recipient}) {
        const auto downloaded = client.Get("/api/files/" + std::to_string(id), running.headers(user));
        REQUIRE(downloaded);
        CHECK(downloaded->status == 200);
        CHECK(downloaded->body == bytes);
        CHECK(downloaded->get_header_value("Content-Type") == "application/octet-stream");
        CHECK(downloaded->get_header_value("Content-Disposition").find(httplib::encode_uri_component(name)) != std::string::npos);
    }
    for (const auto& path : {"/api/files/" + std::to_string(id), std::string("/api/files/18446744073709551615")}) {
        const auto denied = client.Get(path, running.headers(outsider));
        REQUIRE(denied);
        CHECK(denied->status == 404);
        CHECK(json::parse(denied->body)["error"]["code"] == "file_not_found");
    }
    CHECK(fixture.count("files") == 1);
    CHECK(fixture.count("file_recipients") == 1);
    CHECK(fixture.count("messages") == 1);
    CHECK(fixture.count("message_deliveries") == 1);
    const auto pending = fixture.store.pending_messages(recipient);
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].file);
    CHECK(pending[0].file->id == id);
    CHECK(std::to_string(pending[0].id) == data["message_id"]);
    CHECK(fixture.store.pending_messages(owner).empty());

    std::filesystem::remove(running.directory.path / stored->storage_key);
    const auto missing = client.Get("/api/files/" + std::to_string(id), running.headers(owner));
    REQUIRE(missing);
    CHECK(missing->status == 404);
}

TEST_CASE("File endpoints reject invalid tokens and metadata before creating files", "[database][http][file]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    RunningFiles running;
    httplib::Client client("127.0.0.1", running.http.port());
    for (const auto& headers : {httplib::Headers{}, httplib::Headers{{"Authorization", "Bearer invalid"}},
                               running.headers(18446744073709551615ULL)}) {
        const auto denied = client.Post("/api/files", headers, "data", "application/octet-stream");
        REQUIRE(denied);
        CHECK(denied->status == 401);
        CHECK(denied->get_header_value("WWW-Authenticate") == "Bearer");
    }
    const auto self = json::array({std::to_string(owner)});
    for (const auto& name : {std::string(""), std::string("../file"), std::string(".."), std::string("a\\b"),
                             std::string("a\nb"), std::string(256, 'x')}) {
        const auto response = client.Post("/api/files", running.headers(owner, {{"name", name}, {"recipients", self}}),
                                           "data", "application/octet-stream");
        REQUIRE(response);
        CHECK(response->status == 400);
    }
    auto too_many = json::array();
    for (int i = 0; i < 65; ++i) too_many.push_back(std::to_string(owner));
    for (const auto& recipients : {json::array(), json("1"), json::array({1}), json::array({"01"}),
                                   json::array({"0"}), json::array({"18446744073709551616"}),
                                   json::array({std::to_string(owner), "18446744073709551615"}), too_many}) {
        const auto response = client.Post("/api/files", running.headers(owner, {{"name", "file"}, {"recipients", recipients}}),
                                           "data", "application/octet-stream");
        REQUIRE(response);
        CHECK(response->status == 400);
    }
    const auto media = client.Post("/api/files", running.headers(owner), "data", "text/plain");
    REQUIRE(media);
    CHECK(media->status == 415);
    auto malformed = running.headers(owner);
    malformed.erase("X-File-Metadata");
    malformed.emplace("X-File-Metadata", "{");
    const auto bad_json = client.Post("/api/files", malformed, "data", "application/octet-stream");
    REQUIRE(bad_json);
    CHECK(bad_json->status == 400);
    const auto chunked = client.Post("/api/files", running.headers(owner, {{"name", "file"}, {"recipients", self}}),
        [](std::size_t, httplib::DataSink& sink) { sink.write("data", 4); sink.done(); return true; },
        "application/octet-stream");
    REQUIRE(chunked);
    CHECK(chunked->status == 411);
    CHECK(running.file_count() == 0);
    CHECK(fixture.count("files") == 0);
    CHECK(fixture.count("messages") == 0);
}

TEST_CASE("Uploads accept empty files and the exact size limit and clean up incomplete bodies", "[database][http][file]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    RunningFiles running(65536);
    httplib::Client client("127.0.0.1", running.http.port());
    const auto headers = running.headers(owner, {{"name", "file.bin"}, {"recipients", {std::to_string(owner)}}});
    for (const auto size : {0, 65536}) {
        const auto response = client.Post("/api/files", headers, std::string(size, 'x'), "application/octet-stream");
        REQUIRE(response);
        REQUIRE(response->status == 201);
        const auto id = json::parse(response->body)["file_id"].get<std::string>();
        const auto download = client.Get("/api/files/" + id, headers);
        REQUIRE(download);
        CHECK(download->status == 200);
        CHECK(download->body == std::string(size, 'x'));
    }
    const auto oversized = client.Post("/api/files", headers, std::string(65537, 'x'), "application/octet-stream");
    REQUIRE(oversized);
    CHECK(oversized->status == 413);
    CHECK(json::parse(oversized->body)["error"]["code"] == "payload_too_large");
    const auto incomplete = client.Post("/api/files", headers, 100,
        [](std::size_t, std::size_t, httplib::DataSink& sink) { sink.write("short", 5); return false; },
        "application/octet-stream");
    CHECK_FALSE(incomplete);
    running.http.stop();
    CHECK_NOTHROW(running.http.check());
    CHECK(running.file_count() == 2);
    CHECK(fixture.count("files") == 2);
    CHECK(fixture.count("messages") == 2);
}

TEST_CASE("An upload removes disk content when its database transaction fails", "[database][http][file]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    fixture.database.execute("ALTER TABLE files ADD CONSTRAINT reject_test_upload CHECK (size_bytes = 0)");
    struct DropConstraint {
        Database& database;
        ~DropConstraint() { database.execute("ALTER TABLE files DROP CHECK reject_test_upload"); }
    } cleanup{fixture.database};
    RunningFiles running;
    httplib::Client client("127.0.0.1", running.http.port());
    const auto response = client.Post("/api/files", running.headers(owner,
        {{"name", "file.bin"}, {"recipients", {std::to_string(owner)}}}), "complete file", "application/octet-stream");
    REQUIRE(response);
    CHECK(response->status == 500);
    CHECK(json::parse(response->body)["error"]["code"] == "internal_error");
    CHECK(running.file_count() == 0);
    CHECK(fixture.count("files") == 0);
    CHECK(fixture.count("messages") == 0);
}

TEST_CASE("File metadata limits apply before any disk or database writes", "[database][http][file]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    // Exercise the per-file limit below the listener's 4 KiB minimum body limit too.
    RunningFiles running(32);
    httplib::Client client("127.0.0.1", running.http.port());
    const auto recipients = json::array({std::to_string(owner)});
    const auto valid = json{{"name", "file.bin"}, {"recipients", recipients}};
    const std::string deep = R"({"ignored":)" + std::string(64, '[') + "0" + std::string(64, ']') + "}";
    for (const auto& metadata : {std::string("[]"), json{{"name", "file.bin"}}.dump(),
                                json{{"recipients", recipients}}.dump(),
                                json{{"name", "file.bin"}, {"recipients", recipients}, {"ignored", std::string(4097, 'x')}}.dump(), deep}) {
        auto headers = running.headers(owner);
        headers.erase("X-File-Metadata");
        headers.emplace("X-File-Metadata", metadata);
        const auto response = client.Post("/api/files", headers, "data", "application/octet-stream");
        REQUIRE(response);
        CHECK(response->status == 400);
    }
    const auto oversized = client.Post("/api/files", running.headers(owner, valid),
                                       std::string(33, 'x'), "application/octet-stream");
    REQUIRE(oversized);
    CHECK(oversized->status == 413);
    CHECK(running.file_count() == 0);
    CHECK(fixture.count("files") == 0);
    CHECK(fixture.count("messages") == 0);

    const std::string name(255, 'n');
    const auto accepted = client.Post("/api/files", running.headers(owner,
        {{"name", name}, {"recipients", recipients}}), std::string(32, 'x'), "application/octet-stream");
    REQUIRE(accepted);
    REQUIRE(accepted->status == 201);
    CHECK(json::parse(accepted->body)["name"] == name);
    CHECK(running.file_count() == 1);
}

TEST_CASE("File storage errors return errors without stopping later transfers", "[database][http][file]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    TempDirectory directory;
    const auto storage = directory.path / "storage";
    { std::ofstream blocked(storage); blocked << "A file cannot be a storage directory"; }
    TokenService tokens(std::string(32, 'k'));
    AuthService auth(tokens);
    HttpServer server(auth, test_config(), "127.0.0.1", HttpConfig{0, 1}, FileConfig{storage.string(), 1024});
    server.start();
    httplib::Client client("127.0.0.1", server.port());
    const httplib::Headers headers{{"Authorization", "Bearer " + tokens.issue(owner)},
        {"X-File-Metadata", json{{"name", "notes.bin"}, {"recipients", {std::to_string(owner)}}}.dump()}};
    const auto failed = client.Post("/api/files", headers, "bytes", "application/octet-stream");
    REQUIRE(failed);
    CHECK(failed->status == 500);
    CHECK(json::parse(failed->body)["error"]["code"] == "internal_error");
    CHECK(fixture.count("files") == 0);
    CHECK(fixture.count("messages") == 0);

    std::filesystem::remove(storage);
    const auto uploaded = client.Post("/api/files", headers, "bytes", "application/octet-stream");
    REQUIRE(uploaded);
    REQUIRE(uploaded->status == 201);
    const auto id = json::parse(uploaded->body)["file_id"].get<std::string>();
    const auto file = fixture.store.file_for_user(std::stoull(id), owner);
    REQUIRE(file);
    const auto path = storage / file->storage_key;
    { std::ofstream changed(path, std::ios::binary | std::ios::trunc); changed << "shorter metadata mismatch"; }
    const auto mismatch = client.Get("/api/files/" + id, headers);
    REQUIRE(mismatch);
    CHECK(mismatch->status == 500);
    CHECK(json::parse(mismatch->body)["error"]["code"] == "internal_error");
    { std::ofstream restored(path, std::ios::binary | std::ios::trunc); restored << "bytes"; }
    const auto downloaded = client.Get("/api/files/" + id, headers);
    REQUIRE(downloaded);
    CHECK(downloaded->status == 200);
    CHECK(downloaded->body == "bytes");
    server.stop();
    CHECK_NOTHROW(server.check());
}

TEST_CASE("Committed files survive a failure while announcing the upload", "[database][http][file]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    const auto recipient = fixture.store.create_user("recipient", "test-hash");
    TempDirectory directory;
    TokenService tokens(std::string(32, 'k'));
    AuthService auth(tokens);
    HttpServer server(auth, test_config(), "127.0.0.1", HttpConfig{0, 1}, FileConfig{directory.path.string(), 1024},
        [](UserNotification) { throw std::runtime_error("Notification callback failed after commit"); });
    server.start();
    httplib::Client client("127.0.0.1", server.port());
    const httplib::Headers headers{{"Authorization", "Bearer " + tokens.issue(owner)},
        {"X-File-Metadata", json{{"name", "notes.bin"}, {"recipients", {std::to_string(recipient)}}}.dump()}};
    const auto response = client.Post("/api/files", headers, "saved bytes", "application/octet-stream");
    REQUIRE(response);
    CHECK(response->status == 500);
    CHECK(fixture.count("files") == 1);
    CHECK(fixture.count("file_recipients") == 1);
    CHECK(fixture.count("messages") == 1);
    CHECK(fixture.count("message_deliveries") == 1);
    const auto pending = fixture.store.pending_messages(recipient);
    REQUIRE(pending.size() == 1);
    REQUIRE(pending[0].file);
    CHECK(std::filesystem::file_size(directory.path / pending[0].file->storage_key) == 11);
    const auto downloaded = client.Get("/api/files/" + std::to_string(pending[0].file->id),
        httplib::Headers{{"Authorization", "Bearer " + tokens.issue(recipient)}});
    REQUIRE(downloaded);
    CHECK(downloaded->status == 200);
    CHECK(downloaded->body == "saved bytes");
    server.stop();
    CHECK_NOTHROW(server.check());
}
