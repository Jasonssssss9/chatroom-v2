#pragma once

#include "db/chat_store.h"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

namespace {
chatroom::DatabaseConfig test_config() {
    auto config = chatroom::load_config().database;
    if (config.name != "chatroom_test") {
        throw std::runtime_error("Set CHATROOM_DB_NAME=chatroom_test to run database tests");
    }
    return config;
}

// Reset only the explicitly selected, disposable chatroom_test database.
struct DatabaseFixture {
    chatroom::Database database{test_config()};
    chatroom::ChatStore store{database};

    DatabaseFixture() {
        std::ifstream file(CHATROOM_SCHEMA_PATH);
        if (!file) throw std::runtime_error("Cannot open the test database schema");
        std::string statement;
        while (std::getline(file, statement, ';')) {
            if (statement.find_first_not_of(" \r\n\t") != std::string::npos) {
                database.execute(statement);
            }
        }
        for (const auto* table : {"file_recipients", "files", "message_deliveries", "messages",
                                  "group_members", "chat_groups", "users"}) {
            database.execute(std::string("DELETE FROM ") + table);
        }
    }

    std::uint64_t count(const char* table) {
        return std::stoull(database.query(std::string("SELECT COUNT(*) FROM ") + table)[0][0].value());
    }
};
} // namespace
