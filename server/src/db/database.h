#pragma once

#include "config.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mysql/mysql.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace chatroom {

// Initialize before starting workers. Connections also initialize their thread.
void initialize_mysql();

class DatabaseError : public std::runtime_error {
public:
    DatabaseError(unsigned int code, const std::string& operation);
    unsigned int code;
};

using SqlValue = std::variant<std::nullptr_t, std::uint64_t, std::string>;
using SqlRow = std::vector<std::optional<std::string>>;

struct SqlResult {
    std::uint64_t insert_id;
    std::uint64_t affected_rows;
};

// Owns one connection. Create, use, and destroy it on the same worker.
// SQL comes from application code; values use prepared-statement parameters.
class Database {
public:
    explicit Database(const DatabaseConfig& config);
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    SqlResult execute(std::string_view sql, const std::vector<SqlValue>& values = {});
    std::vector<SqlRow> query(std::string_view sql, const std::vector<SqlValue>& values = {});

private:
    friend class Transaction;
    std::unique_ptr<MYSQL, decltype(&mysql_close)> connection_{nullptr, mysql_close};
};

// Rolls back unless commit() succeeds. Transactions must not be nested.
class Transaction {
public:
    explicit Transaction(Database& database);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();

private:
    Database& database_;
    bool committed_ = false;
};

} // namespace chatroom
