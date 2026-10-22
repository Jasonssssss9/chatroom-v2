#pragma once

#include "db/database.h"

namespace chatroom {

struct User {
    std::uint64_t id;
    std::string username;
    std::string password_hash;
};

struct SavedGroupMessage {
    std::uint64_t id;
    std::vector<std::uint64_t> recipients;
};

struct GroupInfo {
    std::uint64_t id;
    std::uint64_t owner_id;
    std::string name;
};

struct FileInfo {
    std::uint64_t id;
    std::uint64_t owner_id;
    std::string storage_key;
    std::string original_name;
    std::string content_type;
    std::uint64_t size_bytes;
};

struct Message {
    std::uint64_t id;
    std::uint64_t sender_id;
    std::string client_message_id;
    std::optional<std::uint64_t> group_id;
    std::string text;
    std::string created_at;
    std::optional<FileInfo> file = std::nullopt;
};

struct SavedFile {
    std::uint64_t id;
    std::uint64_t message_id;
};

// Concrete chat persistence on a caller-owned connection. Use on a worker thread.
// Methods that write several tables own their transaction; do not wrap them in another.
class ChatStore {
public:
    explicit ChatStore(Database& database) : database_(database) {}

    // Receives an already hashed password. Hashing and validation belong to authentication.
    std::uint64_t create_user(const std::string& username, const std::string& password_hash);
    std::optional<User> user_by_name(const std::string& username);
    std::optional<User> user_by_id(std::uint64_t id);
    // Returns matching IDs in ascending order without duplicates.
    std::vector<std::uint64_t> existing_user_ids(const std::vector<std::uint64_t>& ids);

    std::uint64_t create_group(std::uint64_t owner, const std::string& name,
                               const std::vector<std::uint64_t>& members);
    std::vector<std::uint64_t> group_members(std::uint64_t group);
    // Current memberships, ordered by group ID; the cursor is exclusive.
    std::vector<GroupInfo> groups_for_user(std::uint64_t user, std::uint64_t after = 0,
                                          std::uint64_t limit = 64);

    // A sender's client_message_id identifies one immutable outgoing message.
    // Exact retries return its existing ID without creating new deliveries.
    std::uint64_t save_direct_message(std::uint64_t sender, const std::string& client_message_id,
                                      const std::string& text,
                                      const std::vector<std::uint64_t>& recipients);
    // No value means the sender is not a member. Recipients are the original
    // persisted delivery snapshot, including on exact retries.
    std::optional<SavedGroupMessage> save_group_message(std::uint64_t sender,
        const std::string& client_message_id, std::uint64_t group, const std::string& text);
    std::vector<Message> pending_messages(std::uint64_t recipient, std::uint64_t limit = 100);
    // Repeated acknowledgments succeed; users cannot acknowledge another user's delivery.
    bool acknowledge_message(std::uint64_t recipient, std::uint64_t message);

    // Saves metadata, access permissions, and the ACK-able notification atomically.
    SavedFile create_file(std::uint64_t owner, const std::string& storage_key,
                              const std::string& original_name, const std::string& content_type,
                              std::uint64_t size, const std::vector<std::uint64_t>& recipients);
    std::optional<FileInfo> file_for_user(std::uint64_t file, std::uint64_t user);

private:
    // Called inside the direct/group transaction after selecting recipients.
    std::uint64_t insert_message(std::uint64_t sender, const std::string& client_message_id,
                                 std::optional<std::uint64_t> group, const std::string& text,
                                 const std::vector<std::uint64_t>& recipients);
    Database& database_;
};

} // namespace chatroom
