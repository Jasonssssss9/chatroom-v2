#pragma once

#include "auth/token_service.h"
#include "db/chat_store.h"

namespace chatroom {

struct AuthError : std::runtime_error {
    AuthError(int status, std::string code, const std::string& message)
        : std::runtime_error(message), status(status), code(std::move(code)) {}
    int status;
    std::string code;
};

// Stateless business operations; each caller supplies its own worker's store.
class AuthService {
public:
    explicit AuthService(const TokenService& tokens);
    std::uint64_t register_user(ChatStore& store, const std::string& username,
                                const std::string& password) const;
    std::string login(ChatStore& store, const std::string& username,
                      const std::string& password) const;
    std::uint64_t authenticate(ChatStore& store, std::string_view token) const;
    std::uint64_t token_lifetime_seconds() const { return tokens_.lifetime_seconds(); }

private:
    const TokenService& tokens_;
    std::string dummy_hash_;
};

} // namespace chatroom
