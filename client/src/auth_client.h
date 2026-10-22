#pragma once

#include <cstdint>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>

namespace chatroom {

// Blocking HTTP authentication on the input thread; stop() may cancel from the receiver.
// Passwords and returned tokens are not stored or printed by this class.
class AuthClient {
public:
    AuthClient(const std::string& address, std::uint16_t port);
    std::string register_user(const std::string& username, const std::string& password);
    std::string login(const std::string& username, const std::string& password);
    void stop() { client_.stop(); }

private:
    nlohmann::json post(const char* path, const std::string& username,
                        const std::string& password, int expected_status);
    httplib::Client client_;
};

} // namespace chatroom
