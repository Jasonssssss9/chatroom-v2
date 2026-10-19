#pragma once

#include <cstdint>
#include <string>

namespace chatroom {

// Issues HS256 JWTs. Keep this service alive while HTTP workers use it.
class TokenService {
public:
    explicit TokenService(std::string secret, std::uint64_t lifetime_seconds = 3600);
    std::string issue(std::uint64_t user_id) const;
    std::uint64_t lifetime_seconds() const { return lifetime_seconds_; }

private:
    std::string secret_;
    std::uint64_t lifetime_seconds_;
};

} // namespace chatroom
