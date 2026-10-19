#include "db/database.h"

#include <utility>

namespace chatroom {
namespace {
struct MySqlLibrary {
    MySqlLibrary() {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            throw std::runtime_error("Cannot initialize the MySQL client library");
        }
    }
    ~MySqlLibrary() { mysql_library_end(); }
};

struct MySqlThread {
    MySqlThread() {
        if (mysql_thread_init() != 0) {
            throw std::runtime_error("Cannot initialize MySQL thread state");
        }
    }
    ~MySqlThread() { mysql_thread_end(); }
};

// Keeps C API handles and bindings local to one operation.
class Statement {
public:
    Statement(MYSQL* connection, std::string_view sql, const std::vector<SqlValue>& values)
        : statement_(mysql_stmt_init(connection), mysql_stmt_close) {
        if (!statement_) throw DatabaseError(mysql_errno(connection), "Creating statement failed");
        if (mysql_stmt_prepare(get(), sql.data(), sql.size()) != 0) fail("Preparing statement failed");
        if (mysql_stmt_param_count(get()) != values.size()) {
            throw std::invalid_argument("SQL parameter count does not match the statement");
        }
        std::vector<MYSQL_BIND> bindings(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            auto& binding = bindings[i];
            if (const auto* number = std::get_if<std::uint64_t>(&values[i])) {
                binding.buffer_type = MYSQL_TYPE_LONGLONG;
                binding.buffer = const_cast<std::uint64_t*>(number);
                binding.is_unsigned = true;
            } else if (const auto* text = std::get_if<std::string>(&values[i])) {
                binding.buffer_type = MYSQL_TYPE_STRING;
                binding.buffer = const_cast<char*>(text->data());
                binding.buffer_length = text->size();
            } else {
                binding.buffer_type = MYSQL_TYPE_NULL;
            }
        }
        if (!bindings.empty() && mysql_stmt_bind_param(get(), bindings.data()) != 0) {
            fail("Binding statement parameters failed");
        }
        if (mysql_stmt_execute(get()) != 0) fail("Executing statement failed");
    }

    MYSQL_STMT* get() const { return statement_.get(); }

    [[noreturn]] void fail(const char* operation) const {
        throw DatabaseError(mysql_stmt_errno(get()), operation);
    }

    std::vector<SqlRow> rows() {
        const bool update_lengths = true;
        if (mysql_stmt_attr_set(get(), STMT_ATTR_UPDATE_MAX_LENGTH, &update_lengths) != 0 ||
            mysql_stmt_store_result(get()) != 0) {
            fail("Buffering statement results failed");
        }
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> metadata(
            mysql_stmt_result_metadata(get()), mysql_free_result);
        if (!metadata) throw std::invalid_argument("Query does not return a result set");
        const auto count = mysql_num_fields(metadata.get());
        const auto* fields = mysql_fetch_fields(metadata.get());
        std::vector<MYSQL_BIND> bindings(count);
        std::vector<std::vector<char>> buffers(count);
        std::vector<unsigned long> lengths(count);
        auto nulls = std::make_unique<bool[]>(count);
        for (unsigned int i = 0; i < count; ++i) {
            buffers[i].resize(fields[i].max_length + 1);
            bindings[i].buffer_type = MYSQL_TYPE_STRING;
            bindings[i].buffer = buffers[i].data();
            bindings[i].buffer_length = buffers[i].size();
            bindings[i].length = &lengths[i];
            bindings[i].is_null = &nulls[i];
        }
        if (mysql_stmt_bind_result(get(), bindings.data()) != 0) fail("Binding query results failed");
        std::vector<SqlRow> result;
        int status;
        while ((status = mysql_stmt_fetch(get())) == 0) {
            SqlRow row;
            for (unsigned int i = 0; i < count; ++i) {
                if (nulls[i]) row.emplace_back(std::nullopt);
                else row.emplace_back(std::string(buffers[i].data(), lengths[i]));
            }
            result.push_back(std::move(row));
        }
        if (status != MYSQL_NO_DATA) fail("Fetching query results failed");
        return result;
    }

private:
    std::unique_ptr<MYSQL_STMT, decltype(&mysql_stmt_close)> statement_;
};
} // namespace

void initialize_mysql() {
    static MySqlLibrary library;
    // Thread-local cleanup runs before library cleanup at process exit.
    thread_local MySqlThread thread;
}

DatabaseError::DatabaseError(unsigned int error_code, const std::string& operation)
    : std::runtime_error(operation + " (MySQL error " + std::to_string(error_code) + ")"),
      code(error_code) {}

Database::Database(const DatabaseConfig& config) {
    initialize_mysql();
    connection_.reset(mysql_init(nullptr));
    if (!connection_) throw std::runtime_error("Cannot allocate a MySQL connection");
    const unsigned int timeout = 5;
    if (mysql_options(connection_.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0 ||
        mysql_options(connection_.get(), MYSQL_OPT_READ_TIMEOUT, &timeout) != 0 ||
        mysql_options(connection_.get(), MYSQL_OPT_WRITE_TIMEOUT, &timeout) != 0 ||
        mysql_options(connection_.get(), MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
        throw DatabaseError(mysql_errno(connection_.get()), "Configuring connection failed");
    }
    if (!mysql_real_connect(connection_.get(), config.host.c_str(), config.user.c_str(),
                            config.password.c_str(), config.name.c_str(), config.port, nullptr, 0)) {
        throw DatabaseError(mysql_errno(connection_.get()), "Connecting to database failed");
    }
    execute("SET time_zone = '+00:00'");
}

SqlResult Database::execute(std::string_view sql, const std::vector<SqlValue>& values) {
    Statement statement(connection_.get(), sql, values);
    return {mysql_stmt_insert_id(statement.get()), mysql_stmt_affected_rows(statement.get())};
}

std::vector<SqlRow> Database::query(std::string_view sql, const std::vector<SqlValue>& values) {
    return Statement(connection_.get(), sql, values).rows();
}

Transaction::Transaction(Database& database) : database_(database) {
    // Transaction control is a fixed command rather than a prepared statement.
    if (mysql_query(database_.connection_.get(), "START TRANSACTION") != 0) {
        throw DatabaseError(mysql_errno(database_.connection_.get()), "Starting transaction failed");
    }
}

Transaction::~Transaction() {
    if (!committed_) mysql_rollback(database_.connection_.get());
}

void Transaction::commit() {
    if (mysql_commit(database_.connection_.get()) != 0) {
        throw DatabaseError(mysql_errno(database_.connection_.get()), "Committing transaction failed");
    }
    committed_ = true;
}

} // namespace chatroom
