#include "database_fixture.h"

#include <catch2/catch_test_macros.hpp>
#include <future>
#include <limits>

using namespace chatroom;

TEST_CASE("MySQL bindings preserve Unicode, nulls, empty strings, and large values", "[database]") {
    DatabaseFixture fixture;
    const std::string text = std::string(70000, 'x') + u8"\u4f60\u597d \U0001f642" +
                              std::string("\0tail", 5);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto rows = fixture.database.query("SELECT CAST(? AS UNSIGNED), ?, ?, ?",
                                             {maximum, text, nullptr, std::string{}});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0][0] == std::to_string(maximum));
    CHECK(rows[0][1] == text);
    CHECK_FALSE(rows[0][2].has_value());
    CHECK(rows[0][3] == "");
    CHECK_THROWS_AS(fixture.database.query("SELECT ?"), std::invalid_argument);
    const auto settings = fixture.database.query("SELECT @@character_set_connection, @@time_zone");
    CHECK(settings[0][0] == "utf8mb4");
    CHECK(settings[0][1] == "+00:00");
}

TEST_CASE("Users have unique case-sensitive names and literal SQL parameters", "[database]") {
    DatabaseFixture fixture;
    const std::string name = "name' OR 1=1 --";
    const auto id = fixture.store.create_user(name, "test-hash");
    const auto found = fixture.store.user_by_name(name);
    REQUIRE(found.has_value());
    CHECK(found->id == id);
    CHECK(found->password_hash == "test-hash");
    CHECK(fixture.store.user_by_id(id)->username == name);
    CHECK_FALSE(fixture.store.user_by_name("missing"));
    CHECK_FALSE(fixture.store.user_by_id(0));
    CHECK_THROWS_AS(fixture.store.create_user(name, "another-hash"), DatabaseError);
    CHECK(fixture.store.create_user("Alice", "test-hash") !=
          fixture.store.create_user("alice", "test-hash"));
}

TEST_CASE("User ID lookup returns only existing IDs in sorted order", "[database]") {
    DatabaseFixture fixture;
    const auto alice = fixture.store.create_user("alice", "test-hash");
    const auto bob = fixture.store.create_user("bob", "test-hash");
    const auto missing = std::numeric_limits<std::uint64_t>::max();
    CHECK(fixture.store.existing_user_ids({}).empty());
    CHECK(fixture.store.existing_user_ids({alice}) == std::vector<std::uint64_t>{alice});
    CHECK(fixture.store.existing_user_ids({bob, alice, bob}) ==
          std::vector<std::uint64_t>{alice, bob});
    CHECK(fixture.store.existing_user_ids({missing, bob, alice}) ==
          std::vector<std::uint64_t>{alice, bob});
    CHECK(fixture.store.existing_user_ids({missing}).empty());
}

TEST_CASE("Transactions commit explicitly and roll back on exceptions", "[database]") {
    DatabaseFixture fixture;
    try {
        Transaction transaction(fixture.database);
        fixture.store.create_user("rolled-back", "test-hash");
        throw std::runtime_error("Expected application failure");
    } catch (const std::runtime_error&) {}
    CHECK_FALSE(fixture.store.user_by_name("rolled-back"));
    {
        Transaction transaction(fixture.database);
        fixture.store.create_user("committed", "test-hash");
        transaction.commit();
    }
    Database other(test_config());
    CHECK(ChatStore(other).user_by_name("committed").has_value());
}

TEST_CASE("Direct messages persist per-recipient acknowledgments and exact retries", "[database]") {
    DatabaseFixture fixture;
    const auto alice = fixture.store.create_user("alice", "test-hash");
    const auto bob = fixture.store.create_user("bob", "test-hash");
    const auto carol = fixture.store.create_user("carol", "test-hash");
    const std::string text = u8"Hello \U0001f642 'quoted' \\ text";
    const auto id = fixture.store.save_direct_message(alice, "m1", text, {bob, carol, bob});
    const auto pending = fixture.store.pending_messages(bob);
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].id == id);
    CHECK(pending[0].sender_id == alice);
    CHECK(pending[0].text == text);
    CHECK_FALSE(pending[0].group_id);
    CHECK_FALSE(pending[0].created_at.empty());
    CHECK(fixture.count("message_deliveries") == 2);
    CHECK(fixture.store.save_direct_message(alice, "m1", text, {carol, bob}) == id);
    CHECK_FALSE(fixture.store.acknowledge_message(alice, id));
    CHECK(fixture.store.acknowledge_message(bob, id));
    CHECK(fixture.store.acknowledge_message(bob, id));
    CHECK(fixture.store.pending_messages(bob).empty());
    CHECK(fixture.store.pending_messages(carol).size() == 1);
    CHECK(fixture.store.save_direct_message(alice, "m1", text, {bob, carol}) == id);
    CHECK(fixture.store.pending_messages(bob).empty());
    CHECK_THROWS_AS(fixture.store.save_direct_message(alice, "m1", "different", {bob, carol}),
                    std::invalid_argument);
    CHECK_THROWS_AS(fixture.store.save_direct_message(alice, "m1", text, {bob}), std::invalid_argument);
    CHECK_THROWS_AS(fixture.store.save_direct_message(alice, "empty", text, {}), std::invalid_argument);
    CHECK(fixture.count("messages") == 1);
    Database other(test_config());
    CHECK(ChatStore(other).pending_messages(carol)[0].id == id);
}

TEST_CASE("Pending messages use bounded batches and advance after acknowledgment", "[database]") {
    DatabaseFixture fixture;
    const auto sender = fixture.store.create_user("sender", "test-hash");
    const auto recipient = fixture.store.create_user("recipient", "test-hash");
    std::vector<std::uint64_t> ids;
    for (const auto* key : {"m1", "m2", "m3"}) {
        ids.push_back(fixture.store.save_direct_message(sender, key, "text", {recipient}));
    }
    const auto first = fixture.store.pending_messages(recipient, 2);
    REQUIRE(first.size() == 2);
    CHECK(first[0].id == ids[0]);
    CHECK(first[1].id == ids[1]);
    fixture.store.acknowledge_message(recipient, first[0].id);
    fixture.store.acknowledge_message(recipient, first[1].id);
    const auto last = fixture.store.pending_messages(recipient, 2);
    REQUIRE(last.size() == 1);
    CHECK(last[0].id == ids[2]);
    fixture.store.acknowledge_message(recipient, ids[2]);
    CHECK(fixture.store.pending_messages(recipient).empty());
    CHECK(fixture.store.pending_messages(recipient, 0).empty());
}

TEST_CASE("A failed recipient insert rolls back the message and all deliveries", "[database]") {
    DatabaseFixture fixture;
    const auto sender = fixture.store.create_user("sender", "test-hash");
    const auto recipient = fixture.store.create_user("recipient", "test-hash");
    CHECK_THROWS_AS(fixture.store.save_direct_message(sender, "m1", "text",
        {recipient, std::numeric_limits<std::uint64_t>::max()}), DatabaseError);
    CHECK(fixture.count("messages") == 0);
    CHECK(fixture.count("message_deliveries") == 0);
    CHECK(fixture.store.save_direct_message(sender, "m1", "text", {recipient}) != 0);
}

TEST_CASE("Groups include their owner and persist one delivery per other member", "[database]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    const auto member = fixture.store.create_user("member", "test-hash");
    const auto outsider = fixture.store.create_user("outsider", "test-hash");
    const auto group = fixture.store.create_group(owner, "Study group", {member, member, owner});
    CHECK(fixture.store.group_members(group) == std::vector<std::uint64_t>{owner, member});
    CHECK_FALSE(fixture.store.save_group_message(outsider, "bad", group, "text"));
    const auto saved = fixture.store.save_group_message(member, "g1", group, "group text");
    REQUIRE(saved.has_value());
    const auto id = saved->id;
    CHECK(saved->recipients == std::vector<std::uint64_t>{owner});
    REQUIRE(fixture.store.pending_messages(owner).size() == 1);
    CHECK(fixture.store.pending_messages(owner)[0].group_id == group);
    CHECK(fixture.store.pending_messages(member).empty());
    CHECK(fixture.store.save_group_message(member, "g1", group, "group text")->id == id);
    // Retrying a send must not add recipients who joined after the original send.
    fixture.database.execute("INSERT INTO group_members (group_id, user_id) VALUES (?, ?)",
                              {group, outsider});
    const auto retry = fixture.store.save_group_message(member, "g1", group, "group text");
    REQUIRE(retry.has_value());
    CHECK(retry->id == id);
    CHECK(retry->recipients == saved->recipients);
    CHECK(fixture.store.pending_messages(outsider).empty());
    CHECK_THROWS_AS(fixture.store.save_direct_message(member, "g1", "group text", {owner}),
                    std::invalid_argument);
}

TEST_CASE("A group with a missing member is rolled back", "[database]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    CHECK_THROWS_AS(fixture.store.create_group(owner, "Invalid group",
        {std::numeric_limits<std::uint64_t>::max()}), DatabaseError);
    CHECK(fixture.count("chat_groups") == 0);
    CHECK(fixture.count("group_members") == 0);
}

TEST_CASE("File metadata is accessible only to the owner and selected recipients", "[database]") {
    DatabaseFixture fixture;
    const auto owner = fixture.store.create_user("owner", "test-hash");
    const auto recipient = fixture.store.create_user("recipient", "test-hash");
    const auto outsider = fixture.store.create_user("outsider", "test-hash");
    const std::uint64_t size = 5ULL * 1024 * 1024 * 1024;
    const auto id = fixture.store.create_file(owner, "generated-key", "notes 'final'.txt",
                                              "text/plain", size, {recipient, recipient});
    const auto file = fixture.store.file_for_user(id, recipient);
    REQUIRE(file.has_value());
    CHECK(file->storage_key == "generated-key");
    CHECK(file->original_name == "notes 'final'.txt");
    CHECK(file->content_type == "text/plain");
    CHECK(file->size_bytes == size);
    CHECK(fixture.store.file_for_user(id, owner).has_value());
    CHECK_FALSE(fixture.store.file_for_user(id, outsider));
    CHECK_FALSE(fixture.store.file_for_user(0, owner));
    CHECK_THROWS_AS(fixture.store.create_file(owner, "bad-key", "file", "text/plain", 1,
        {recipient, std::numeric_limits<std::uint64_t>::max()}), DatabaseError);
    CHECK(fixture.count("files") == 1);
    CHECK(fixture.count("file_recipients") == 1);
}

TEST_CASE("Independent worker connections deduplicate concurrent message retries", "[database]") {
    DatabaseFixture fixture;
    const auto sender = fixture.store.create_user("sender", "test-hash");
    const auto recipient = fixture.store.create_user("recipient", "test-hash");
    const auto config = test_config();
    std::promise<void> start;
    auto ready = start.get_future().share();
    std::vector<std::future<std::uint64_t>> results;
    for (int i = 0; i < 4; ++i) {
        results.push_back(std::async(std::launch::async, [=] {
            Database database(config);
            ChatStore store(database);
            ready.wait();
            return store.save_direct_message(sender, "same-key", "text", {recipient});
        }));
    }
    start.set_value();
    const auto id = results.front().get();
    for (std::size_t i = 1; i < results.size(); ++i) CHECK(results[i].get() == id);
    CHECK(fixture.count("messages") == 1);
    CHECK(fixture.count("message_deliveries") == 1);
}

TEST_CASE("Connection errors report codes without including credentials", "[database]") {
    auto config = test_config();
    config.password = "deliberately-incorrect-test-password";
    try {
        Database database(config);
        FAIL("Incorrect credentials were accepted");
    } catch (const DatabaseError& error) {
        CHECK(error.code == 1045);
        CHECK(std::string(error.what()).find(config.password) == std::string::npos);
    }
}
