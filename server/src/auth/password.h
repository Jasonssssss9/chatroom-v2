#pragma once

#include <string>
#include <string_view>

namespace chatroom {

// Uses OpenSSL scrypt with a fresh random salt. The encoded hash includes its parameters.
std::string hash_password(std::string_view password);
bool verify_password(std::string_view password, std::string_view encoded);

} // namespace chatroom
