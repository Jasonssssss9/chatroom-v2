#include "auth/password.h"
#include "auth/token_service.h"

#include <catch2/catch_test_macros.hpp>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>

using namespace chatroom;

TEST_CASE("Password hashes use random salts and verify the complete password", "[auth]") {
    const std::string password = u8"password \U0001f642" + std::string("\0tail", 5);
    const auto first = hash_password(password);
    const auto second = hash_password(password);
    CHECK(first != second);
    CHECK(first.size() < 255);
    CHECK(first.find(password) == std::string::npos);
    CHECK(verify_password(password, first));
    CHECK_FALSE(verify_password("different-password", first));
    CHECK_FALSE(verify_password(password.substr(0, password.size() - 1), first));
}

TEST_CASE("Invalid stored password hashes are rejected", "[auth]") {
    const auto hash = hash_password("valid-password");
    auto invalid_hex = hash;
    invalid_hex.back() = 'z';
    auto different_parameters = hash;
    different_parameters[7] = '9';
    for (const auto& value : {std::string{}, std::string("test-hash"), hash.substr(1), invalid_hex,
                              different_parameters}) {
        CHECK_FALSE(verify_password("valid-password", value));
    }
}

TEST_CASE("Issued JWTs have a valid signature, user subject, issuer, and expiration", "[auth]") {
    const std::string secret(32, 'k');
    const TokenService tokens(secret, 120);
    const auto before = std::chrono::system_clock::now();
    const auto token = tokens.issue(42);
    const auto decoded = jwt::decode(token);
    CHECK_NOTHROW(jwt::verify().allow_algorithm(jwt::algorithm::hs256{secret})
                      .with_issuer("chatroom-v2").with_type("JWT").verify(decoded));
    CHECK(decoded.get_subject() == "42");
    CHECK(decoded.get_algorithm() == "HS256");
    CHECK(decoded.get_expires_at() - decoded.get_issued_at() == std::chrono::seconds(120));
    CHECK(decoded.get_issued_at() >= before - std::chrono::seconds(1));
    CHECK_FALSE(decoded.has_payload_claim("password"));
    CHECK_FALSE(decoded.has_payload_claim("password_hash"));
}

TEST_CASE("JWT configuration rejects short secrets and invalid lifetimes", "[auth]") {
    CHECK_THROWS_AS(TokenService(""), std::invalid_argument);
    CHECK_THROWS_AS(TokenService(std::string(31, 'k')), std::invalid_argument);
    CHECK_THROWS_AS(TokenService(std::string(32, 'k'), 0), std::invalid_argument);
    CHECK_THROWS_AS(TokenService(std::string(32, 'k'), 86401), std::invalid_argument);
}
