#include "db/chat_store.h"

#include <algorithm>
#include <set>

namespace chatroom {
namespace {
std::uint64_t number(const std::optional<std::string>& value) {
    return std::stoull(value.value());
}

std::optional<User> read_user(const std::vector<SqlRow>& rows) {
    if (rows.empty()) return std::nullopt;
    const auto& row = rows.front();
    return User{number(row[0]), row[1].value(), row[2].value()};
}

Message read_message(const SqlRow& row) {
    return {number(row[0]), number(row[1]), row[2].value(),
            row[3] ? std::optional<std::uint64_t>(number(row[3])) : std::nullopt,
            row[4].value(), row[5].value()};
}

std::vector<std::uint64_t> read_ids(const std::vector<SqlRow>& rows) {
    std::vector<std::uint64_t> ids;
    for (const auto& row : rows) ids.push_back(number(row[0]));
    return ids;
}
} // namespace

std::uint64_t ChatStore::create_user(const std::string& username, const std::string& password_hash) {
    return database_.execute("INSERT INTO users (username, password_hash) VALUES (?, ?)",
                              {username, password_hash}).insert_id;
}

std::optional<User> ChatStore::user_by_name(const std::string& username) {
    return read_user(database_.query("SELECT id, username, password_hash FROM users WHERE username = ?",
                                     {username}));
}

std::optional<User> ChatStore::user_by_id(std::uint64_t id) {
    return read_user(database_.query("SELECT id, username, password_hash FROM users WHERE id = ?", {id}));
}

std::vector<std::uint64_t> ChatStore::existing_user_ids(const std::vector<std::uint64_t>& ids) {
    if (ids.empty()) return {};
    std::string sql = "SELECT id FROM users WHERE id IN (";
    std::vector<SqlValue> parameters;
    for (const auto id : ids) {
        if (!parameters.empty()) sql += ", ";
        sql += '?';
        parameters.emplace_back(id);
    }
    return read_ids(database_.query(sql + ") ORDER BY id", parameters));
}

std::uint64_t ChatStore::create_group(std::uint64_t owner, const std::string& name,
                                     const std::vector<std::uint64_t>& members) {
    Transaction transaction(database_);
    const auto id = database_.execute("INSERT INTO chat_groups (owner_id, name) VALUES (?, ?)",
                                      {owner, name}).insert_id;
    std::set<std::uint64_t> unique(members.begin(), members.end());
    unique.insert(owner);
    for (const auto member : unique) {
        database_.execute("INSERT INTO group_members (group_id, user_id) VALUES (?, ?)", {id, member});
    }
    transaction.commit();
    return id;
}

std::vector<std::uint64_t> ChatStore::group_members(std::uint64_t group) {
    return read_ids(database_.query(
        "SELECT user_id FROM group_members WHERE group_id = ? ORDER BY user_id", {group}));
}

std::uint64_t ChatStore::insert_message(std::uint64_t sender, const std::string& client_message_id,
                                       std::optional<std::uint64_t> group, const std::string& text,
                                       const std::vector<std::uint64_t>& recipients) {
    const std::set<std::uint64_t> unique(recipients.begin(), recipients.end());
    const SqlValue target = group ? SqlValue(*group) : SqlValue(nullptr);
    const auto result = database_.execute(
        "INSERT INTO messages (sender_id, client_message_id, group_id, text) VALUES (?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE id = LAST_INSERT_ID(id)", {sender, client_message_id, target, text});
    const auto id = result.insert_id;
    if (result.affected_rows != 1) {
        // The upsert locks the existing row; this read also avoids an older snapshot.
        const auto rows = database_.query(
            "SELECT id, sender_id, client_message_id, group_id, text, created_at FROM messages "
            "WHERE id = ? FOR UPDATE", {id});
        const auto existing = read_message(rows.at(0));
        if (existing.group_id != group || existing.text != text) {
            throw std::invalid_argument("Client message ID belongs to a different message");
        }
        if (!group) {
            const auto delivered_to = read_ids(database_.query(
                "SELECT recipient_id FROM message_deliveries WHERE message_id = ? "
                "ORDER BY recipient_id FOR SHARE", {existing.id}));
            if (delivered_to != std::vector<std::uint64_t>(unique.begin(), unique.end())) {
                throw std::invalid_argument("Client message ID belongs to different recipients");
            }
        }
        return existing.id;
    }
    for (const auto recipient : unique) {
        database_.execute("INSERT INTO message_deliveries (message_id, recipient_id) VALUES (?, ?)",
                           {id, recipient});
    }
    return id;
}

std::uint64_t ChatStore::save_direct_message(std::uint64_t sender,
                                            const std::string& client_message_id,
                                            const std::string& text,
                                            const std::vector<std::uint64_t>& recipients) {
    if (recipients.empty()) throw std::invalid_argument("A direct message needs at least one recipient");
    Transaction transaction(database_);
    const auto id = insert_message(sender, client_message_id, std::nullopt, text, recipients);
    transaction.commit();
    return id;
}

std::optional<SavedGroupMessage> ChatStore::save_group_message(std::uint64_t sender,
                                           const std::string& client_message_id,
                                           std::uint64_t group, const std::string& text) {
    Transaction transaction(database_);
    auto members = read_ids(database_.query(
        "SELECT user_id FROM group_members WHERE group_id = ? ORDER BY user_id FOR SHARE", {group}));
    const auto sender_it = std::find(members.begin(), members.end(), sender);
    if (sender_it == members.end()) return std::nullopt;
    members.erase(sender_it);
    const auto id = insert_message(sender, client_message_id, group, text, members);
    const auto recipients = read_ids(database_.query(
        "SELECT recipient_id FROM message_deliveries WHERE message_id = ? "
        "ORDER BY recipient_id FOR SHARE", {id}));
    transaction.commit();
    return SavedGroupMessage{id, recipients};
}

std::vector<Message> ChatStore::pending_messages(std::uint64_t recipient, std::uint64_t limit) {
    const auto rows = database_.query(
        "SELECT m.id, m.sender_id, m.client_message_id, m.group_id, m.text, m.created_at "
        "FROM message_deliveries AS d JOIN messages AS m ON m.id = d.message_id "
        "WHERE d.recipient_id = ? AND d.acknowledged_at IS NULL "
        "ORDER BY m.id LIMIT ?", {recipient, limit});
    std::vector<Message> messages;
    for (const auto& row : rows) messages.push_back(read_message(row));
    return messages;
}

bool ChatStore::acknowledge_message(std::uint64_t recipient, std::uint64_t message) {
    const auto result = database_.execute(
        "UPDATE message_deliveries SET acknowledged_at = CURRENT_TIMESTAMP(6) "
        "WHERE message_id = ? AND recipient_id = ? AND acknowledged_at IS NULL", {message, recipient});
    return result.affected_rows != 0 || !database_.query(
        "SELECT message_id FROM message_deliveries WHERE message_id = ? AND recipient_id = ?",
        {message, recipient}).empty();
}

std::uint64_t ChatStore::create_file(std::uint64_t owner, const std::string& storage_key,
                                    const std::string& original_name, const std::string& content_type,
                                    std::uint64_t size, const std::vector<std::uint64_t>& recipients) {
    Transaction transaction(database_);
    const auto id = database_.execute(
        "INSERT INTO files (owner_id, storage_key, original_name, content_type, size_bytes) "
        "VALUES (?, ?, ?, ?, ?)", {owner, storage_key, original_name, content_type, size}).insert_id;
    const std::set<std::uint64_t> unique(recipients.begin(), recipients.end());
    for (const auto recipient : unique) {
        database_.execute("INSERT INTO file_recipients (file_id, user_id) VALUES (?, ?)", {id, recipient});
    }
    transaction.commit();
    return id;
}

std::optional<FileInfo> ChatStore::file_for_user(std::uint64_t file, std::uint64_t user) {
    const auto rows = database_.query(
        "SELECT id, owner_id, storage_key, original_name, content_type, size_bytes FROM files "
        "WHERE id = ? AND (owner_id = ? OR EXISTS "
        "(SELECT 1 FROM file_recipients WHERE file_id = files.id AND user_id = ?))", {file, user, user});
    if (rows.empty()) return std::nullopt;
    const auto& row = rows.front();
    return FileInfo{number(row[0]), number(row[1]), row[2].value(), row[3].value(), row[4].value(),
                    number(row[5])};
}

} // namespace chatroom
