#include "auth/token_service.h"

#include <chrono>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <stdexcept>
#include <utility>

namespace chatroom {

TokenService::TokenService(std::string secret, std::uint64_t lifetime_seconds)
    : secret_(std::move(secret)), lifetime_seconds_(lifetime_seconds) {
    if (secret_.size() < 32) {
        throw std::invalid_argument("CHATROOM_JWT_SECRET must contain at least 32 bytes");
    }
    if (lifetime_seconds_ == 0 || lifetime_seconds_ > 86400) {
        throw std::invalid_argument("JWT lifetime must be between 1 and 86400 seconds");
    }
}

std::string TokenService::issue(std::uint64_t user_id) const {
    const auto now = std::chrono::system_clock::now();
    return jwt::create().set_type("JWT").set_issuer("chatroom-v2")
        .set_subject(std::to_string(user_id)).set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(lifetime_seconds_))
        .sign(jwt::algorithm::hs256{secret_});
}

} // namespace chatroom
