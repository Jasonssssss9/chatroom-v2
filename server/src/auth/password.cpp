#include "auth/password.h"

#include <array>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdexcept>

namespace chatroom {
namespace {
constexpr std::string_view kPrefix = "scrypt$32768$8$3$";
using Salt = std::array<unsigned char, 16>;
using Key = std::array<unsigned char, 32>;

std::string hex(const unsigned char* bytes, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < size; ++i) {
        result += digits[bytes[i] >> 4];
        result += digits[bytes[i] & 15];
    }
    return result;
}

bool unhex(std::string_view text, unsigned char* bytes, std::size_t size) {
    if (text.size() != size * 2) return false;
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < size; ++i) {
        const int high = digit(text[i * 2]);
        const int low = digit(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

Key derive(std::string_view password, const Salt& salt) {
    Key key{};
    if (EVP_PBE_scrypt(password.data(), password.size(), salt.data(), salt.size(),
                       32768, 8, 3, 64 * 1024 * 1024, key.data(), key.size()) != 1) {
        throw std::runtime_error("Password hashing failed");
    }
    return key;
}
} // namespace

std::string hash_password(std::string_view password) {
    Salt salt{};
    if (RAND_bytes(salt.data(), salt.size()) != 1) {
        throw std::runtime_error("Password salt generation failed");
    }
    const auto key = derive(password, salt);
    return std::string(kPrefix) + hex(salt.data(), salt.size()) + "$" + hex(key.data(), key.size());
}

bool verify_password(std::string_view password, std::string_view encoded) {
    constexpr std::size_t salt_length = Salt{}.size() * 2;
    if (encoded.size() != kPrefix.size() + salt_length + 1 + Key{}.size() * 2 ||
        encoded.substr(0, kPrefix.size()) != kPrefix || encoded[kPrefix.size() + salt_length] != '$') {
        return false;
    }
    Salt salt{};
    Key expected{};
    if (!unhex(encoded.substr(kPrefix.size(), salt_length), salt.data(), salt.size()) ||
        !unhex(encoded.substr(kPrefix.size() + salt_length + 1), expected.data(), expected.size())) {
        return false;
    }
    const auto actual = derive(password, salt);
    return CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
}

} // namespace chatroom
