#pragma once

#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace chatroom {

// A verified user identity. TCP sessions retain the token's expiration time.
struct Session {
    std::uint64_t user_id;
    std::chrono::system_clock::time_point expires_at;

    bool expired() const { return std::chrono::system_clock::now() >= expires_at; }
};

// Issues and verifies HS256 JWTs. Safe to share between HTTP and TCP workers.
class TokenService {
public:
    explicit TokenService(std::string secret, std::uint64_t lifetime_seconds = 3600);
    std::string issue(std::uint64_t user_id) const;
    // Invalid tokens return no identity; no token contents appear in errors or logs.
    std::optional<Session> verify(std::string_view token) const;
    std::uint64_t lifetime_seconds() const { return lifetime_seconds_; }

private:
    std::string secret_;
    std::uint64_t lifetime_seconds_;
};

} // namespace chatroom
