#include "auth/auth_service.h"
#include "auth/password.h"

namespace chatroom {
namespace {
void validate_credentials(const std::string& username, const std::string& password) {
    if (username.empty() || username.size() > 64) {
        throw AuthError(400, "invalid_request", "username must contain 1 to 64 UTF-8 bytes");
    }
    if (password.size() < 8 || password.size() > 128) {
        throw AuthError(400, "invalid_request", "password must contain 8 to 128 UTF-8 bytes");
    }
}
} // namespace

AuthService::AuthService(const TokenService& tokens)
    : tokens_(tokens), dummy_hash_(hash_password("unused-login-placeholder")) {}

std::uint64_t AuthService::register_user(ChatStore& store, const std::string& username,
                                        const std::string& password) const {
    validate_credentials(username, password);
    const auto hash = hash_password(password);
    try {
        return store.create_user(username, hash);
    } catch (const DatabaseError& error) {
        if (error.code == 1062) throw AuthError(409, "username_taken", "Username is already registered");
        throw;
    }
}

std::string AuthService::login(ChatStore& store, const std::string& username,
                              const std::string& password) const {
    validate_credentials(username, password);
    const auto user = store.user_by_name(username);
    // Unknown users still pay the password verification cost and receive the same error.
    const bool valid = verify_password(password, user ? user->password_hash : dummy_hash_);
    if (!user || !valid) throw AuthError(401, "invalid_credentials", "Invalid username or password");
    return tokens_.issue(user->id);
}

std::uint64_t AuthService::authenticate(ChatStore& store, std::string_view token) const {
    const auto session = tokens_.verify(token);
    if (!session || !store.user_by_id(session->user_id)) {
        throw AuthError(401, "invalid_token", "Token is invalid or expired");
    }
    return session->user_id;
}

} // namespace chatroom
