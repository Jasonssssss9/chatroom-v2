#pragma once

#include <filesystem>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace chatroom {

// Blocking, streamed HTTP transfers on the console input thread.
// The caller supplies the token from a successful TCP login.
class FileClient {
public:
    FileClient(const std::string& address, std::uint16_t port);
    nlohmann::json upload(const std::string& token, const std::filesystem::path& path,
                          nlohmann::json recipients);
    void download(const std::string& token, const std::string& id, const std::filesystem::path& path);

private:
    httplib::Client client_;
};

} // namespace chatroom
