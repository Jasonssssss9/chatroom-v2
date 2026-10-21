#include "auth/password.h"
#include "auth/token_service.h"
#include "session_manager.h"

#include <catch2/catch_test_macros.hpp>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <limits>

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

TEST_CASE("Token verification returns the signed identity and expiry", "[auth][session]") {
    const TokenService tokens(std::string(32, 'k'));
    for (const auto id : {std::uint64_t{1}, std::numeric_limits<std::uint64_t>::max()}) {
        const auto issued = tokens.issue(id);
        const auto session = tokens.verify(issued);
        REQUIRE(session);
        CHECK(session->user_id == id);
        CHECK(session->expires_at == jwt::decode(issued).get_expires_at());
        CHECK_FALSE(session->expired());
    }
}

TEST_CASE("Token verification rejects invalid signatures, algorithms, and claims", "[auth]") {
    using nlohmann::json;
    const std::string secret(32, 'k');
    const TokenService tokens(secret);
    const auto original = tokens.issue(42);
    const json valid = jwt::decode(original).get_payload_json();
    const auto sign = [&](const json& claims, const std::string& type = "JWT") {
        auto builder = jwt::create().set_type(type);
        for (const auto& item : claims.items()) builder.set_payload_claim(item.key(), jwt::claim(item.value()));
        return builder.sign(jwt::algorithm::hs256{secret});
    };
    CHECK_FALSE(tokens.verify(TokenService(std::string(32, 'x')).issue(42)));
    CHECK_FALSE(tokens.verify(jwt::create().sign(jwt::algorithm::none{})));
    CHECK_FALSE(tokens.verify(sign(valid, "OTHER")));
    for (const auto* name : {"iss", "sub", "iat", "exp"}) {
        auto claims = valid;
        claims.erase(name);
        CHECK_FALSE(tokens.verify(sign(claims)));
    }
    auto claims = valid;
    claims["iss"] = "another-service";
    CHECK_FALSE(tokens.verify(sign(claims)));
    for (const auto& subject : {json(""), json("0"), json("01"), json("+1"), json("-1"),
                                json("42tail"), json("18446744073709551616"), json(42)}) {
        claims = valid;
        claims["sub"] = subject;
        CHECK_FALSE(tokens.verify(sign(claims)));
    }
    for (const auto* name : {"iat", "exp", "nbf"}) {
        for (const auto& value : {json(-1), json("123"), json(1.5), json(nullptr),
                                  json(std::numeric_limits<std::uint64_t>::max())}) {
            claims = valid;
            claims[name] = value;
            CHECK_FALSE(tokens.verify(sign(claims)));
        }
    }
    claims = valid;
    claims["iat"] = valid["iat"].get<std::int64_t>() + 120;
    CHECK_FALSE(tokens.verify(sign(claims)));
    claims = valid;
    claims["nbf"] = valid["iat"].get<std::int64_t>() + 120;
    CHECK_FALSE(tokens.verify(sign(claims)));
    claims = valid;
    claims["iat"] = 1;
    claims["exp"] = 2;
    CHECK_FALSE(tokens.verify(sign(claims)));
    for (const auto& value : {std::string{}, std::string("broken"), original + ".extra",
                              std::string(4097, 'x')}) {
        CHECK_FALSE(tokens.verify(value));
    }
}

TEST_CASE("Encoded JWT JSON respects the shared nesting limit", "[auth][protocol]") {
    const TokenService tokens(std::string(32, 'k'));
    const auto encode = [](const std::string& value) {
        return jwt::base::trim<jwt::alphabet::base64url>(jwt::base::encode<jwt::alphabet::base64url>(value));
    };
    const std::string deep = "{\"ignored\":" + std::string(1000, '[') + "0" + std::string(1000, ']') + "}";
    CHECK_FALSE(tokens.verify(encode(deep) + "." + encode("{}") + ".AA"));
    CHECK_FALSE(tokens.verify(encode(R"({"alg":"HS256","typ":"JWT"})") + "." + encode(deep) + ".AA"));
}

TEST_CASE("Session replacement and repeated disconnect preserve the current binding", "[session]") {
    SessionManager sessions;
    const Session alice{7, std::chrono::system_clock::now() + std::chrono::hours(1)};
    CHECK_FALSE(sessions.session(10));
    CHECK_FALSE(sessions.connection_for(7));
    CHECK_FALSE(sessions.bind(10, alice));
    CHECK(sessions.connection_for(7) == 10);
    CHECK_FALSE(sessions.bind(10, alice));
    CHECK(sessions.bind(11, alice) == 10);
    sessions.disconnect(10);
    sessions.disconnect(10);
    CHECK_FALSE(sessions.session(10));
    CHECK(sessions.connection_for(7) == 11);
    sessions.disconnect(11);
    CHECK_FALSE(sessions.connection_for(7));
}

TEST_CASE("Expired sessions cannot be used for online routing", "[session]") {
    SessionManager sessions;
    sessions.bind(10, Session{7, std::chrono::system_clock::now() - std::chrono::seconds(1)});
    CHECK_FALSE(sessions.connection_for(7));
    REQUIRE(sessions.session(10));
    CHECK(sessions.session(10)->expired());
}
