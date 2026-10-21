#include "auth/token_service.h"
#include "protocol.h"

#include <charconv>
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

std::optional<Session> TokenService::verify(std::string_view token) const {
    if (token.empty() || token.size() > 4096) return std::nullopt;
    try {
        // Bound the decoded header and payload before jwt-cpp parses their JSON.
        const auto decoded = jwt::decode(std::string(token), [part = 0](const std::string& value) mutable {
            auto bytes = jwt::base::decode<jwt::alphabet::base64url>(
                jwt::base::pad<jwt::alphabet::base64url>(value));
            if (part++ < 2) parse_json(bytes);
            return bytes;
        });
        if (!decoded.has_issued_at() || !decoded.has_expires_at()) return std::nullopt;
        const nlohmann::json claims = decoded.get_payload_json();
        // Reject dates that cannot be converted to system_clock without overflow.
        const auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::duration::max()).count();
        for (const auto* name : {"iat", "exp", "nbf"}) {
            if (!claims.contains(name)) continue;
            const auto& value = claims.at(name);
            if (!value.is_number_integer() || value < 0 || value > max_seconds) return std::nullopt;
        }
        jwt::verify().allow_algorithm(jwt::algorithm::hs256{secret_})
            .with_issuer("chatroom-v2").with_type("JWT").verify(decoded);

        const auto subject = decoded.get_subject();
        std::uint64_t user_id = 0;
        const auto result = std::from_chars(subject.data(), subject.data() + subject.size(), user_id);
        if (result.ec != std::errc{} || result.ptr != subject.data() + subject.size() ||
            user_id == 0 || subject.front() == '0') return std::nullopt;
        const Session session{user_id, decoded.get_expires_at()};
        if (session.expired() || decoded.get_issued_at() >= session.expires_at) return std::nullopt;
        return session;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace chatroom
